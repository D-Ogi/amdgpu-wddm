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
#include "fixture-raylib.h"
#include "fixture-raylib-b.h"
#include "fixture-rayquery.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

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
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
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
    {
        // A recording list of a COPY queue: D3D12 records these commands in DIRECT and COMPUTE lists only.
        Recording copy_list;
        const HRESULT hr_c = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COPY, copy_list);
        const uint32_t before = device.shell.list_errors;
        device.shell.last_list_error = S_OK;
        device.shell.last_list = nullptr;
        if (hr_c == S_OK) env.lists[copy_list.table].pfnBuildRaytracingAccelerationStructure(copy_list.hlist(), &build);
        checkf(hr_c == S_OK && device.shell.list_errors == before + 1 && device.shell.last_list_error == E_INVALIDARG &&
                   device.shell.last_list == copy_list.rtlist().handle,
               "raytracing: a build on a recording COPY list: one E_INVALIDARG on that list (hr %08lx, %u errors, last "
               "%08lx)",
               static_cast<unsigned long>(hr_c), device.shell.list_errors - before,
               static_cast<unsigned long>(device.shell.last_list_error));
        device.shell.list_errors = before;
        if (hr_c == S_OK) env.lists[copy_list.table].pfnCloseCommandList(copy_list.hlist());
        destroy_recording(env, device, copy_list);
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

// ---- Round trip 9: a ray tracing pipeline ----------------------------------------------------------------------------
// State objects through the DDI (D105-D107, D110-D113), SetPipelineState1 and DispatchRays (L63, L64), over the scene
// of round trip 8: the fixture library (fixture-raylib.h, lib_6_3) with raygen, miss and closest, one triangles hit
// group whose local root signature holds one 32-bit constant, and a global root signature SRV(t0) UAV(u0) and one
// 32-bit constant (2, which miss writes).
//   1. CreateStateObject from the description in the DDI form (describe, below): S_OK and no device error.
//   2. Shader identifiers of raygen, miss and the hit group: 32 bytes each, not all zero, all three different; stack
//      sizes of the three answered, UINT_MAX for an unknown export, the pipeline stack size set and read back.
//   3. A shader table in the UPLOAD buffer (the hit group record: identifier, then the constant), DispatchRays 8x8:
//      every hit writes the constant and every miss 2, the 64 words equal to the pattern computed from the triangle.
//   4. Export identity: the library as the whole dxc container (every export, so the engine knows each by its plain
//      and its mangled name), with a decoy export that shares the plain name "closest", has a mangled name of its own
//      and is associated with a second, different local root signature. The associations must name closest by its
//      mangled name: by the plain one, both local root signatures would reach closest and the engine would give it
//      none. A second DispatchRays with this pipeline writes its own constant on every hit, the 64 words exact; the
//      decoy's local root signature has closest's register one word later, where the record holds another value, so
//      the hits show closest did not take it. The decoy has no DXIL function behind it: this is the naming of the
//      associations, not a selection among real overloads.
//      The description handed to the engine (the state object observer, below) associates closest's local root
//      signature with closest's mangled name alone and the decoy's with the decoy's.
//   5. An absent local root association stays absent: closest's local root signature is the trap (constants in b0
//      space1 and b0 space4, which no global parameter has); the summary associates no local root signature with
//      raygen and miss. The description handed to the engine declares the context's empty local root signature last,
//      with an association of no export (the explicit default), and associates the trap with closest alone; without
//      it the engine's declared default, the trap, would reach raygen and miss. The first create's description shows
//      the same. A third DispatchRays with the empty default in place writes 2 on every miss and the trap's first
//      constant on every hit, the 64 words exact; the pixels alone do not tell whether raygen and miss took the trap,
//      whose registers they do not read.
//   6. Mixed libraries: fixture-raylib listing closest by its mangled name alone, beside fixture-raylib-b with no
//      export list, in both orders. The description handed to the engine: the libraries in that order, the local root
//      signature's association naming closest by that mangled name, the only one the engine knows it by, and the
//      global one naming raygen, miss, that mangled name and miss_far. A dispatch per order writes its own constant on
//      every hit, the 64 words exact.
//   7. A collection-only executable: a COLLECTION of the library, then a pipeline with EXISTING_COLLECTION of all its
//      exports and no DXIL library. The description handed to the engine: the collection's engine object with
//      NumExports 0, no library, the global root signature associated with raygen, miss and closest, the names the
//      collection exposed. The same import with an export list is E_NOTIMPL before the engine (temporarily
//      unsupported, INTEGRATION.md). The executable's record holds one public reference to the collection's engine
//      object, and the collection's DDI object is destroyed before the executable is used: a dispatch through its
//      table writes its own constant on every hit and 2 on every miss, the 64 words exact.
//   8. Growth (D115, D116): a pipeline allowing additions, its identifiers taken and its pipeline stack size set to
//      4096 above its computed one, grown by fixture-raylib-b listing miss_far. The child's stack size read before any
//      set is the parent's setting; the child's raygen identifier is the parent's. The description handed to the
//      engine: grown from the parent's engine object, one library listing one export, the global root signature
//      associated with miss_far alone. Refused growth, one E_INVALIDARG each and no device error: from a pipeline
//      without ALLOW_STATE_OBJECT_ADDITIONS (the engine's check, one engine call) and an addition exporting miss (the
//      bridge's check, no engine call). The child's record holds one public reference to the parent's engine object,
//      and the parent's DDI object is destroyed before the child is used: a dispatch through a table of the parent's
//      raygen and hit group identifiers and miss_far's writes its own constant on every hit and miss_far's 3 on every
//      miss, the 64 words exact.
//   9. Indirect DispatchRays (L64 through ExecuteIndirect): a command signature of one DISPATCH_RAYS argument, stride
//      128, no root signature, created S_OK. Two records over the first pipeline's table, 8x4 then 8x8, and the count
//      words 1, 2, 0 and 3; six variants (max 1; max 2 with count 1, 2, 0 or 3; max 2 without a count buffer), each
//      into its own prefilled result, first from the UPLOAD buffer with no INDIRECT_ARGUMENT transition before them,
//      then from a DEFAULT copy behind one. Every record up to the smaller of count and maximum is traced: rows 0 to 3
//      for max 1 and for count 1, none for count 0, all for the rest, the other rows at the prefill. One record without
//      a count buffer must be exact on any engine; an engine that traces the first record alone and nothing with a
//      count buffer (vkd3d-proton upstream) gives one SKIP line instead.
// The descriptions are copied by the harness's state object observer immediately before the engine's
// CreateStateObject or AddToStateObject (internal.h, harness_set_state_object_observer; the shell's DLL has no
// observer).
// state_object_shell, which the shell's entry thunk resolves these slots' device with, names the device's shell for a
// live state object and for the inert record of a refused create, and nothing once either is destroyed.
// Then refusals, each one error of the expected HRESULT: a subobject of an unknown type and a ray tracing pipeline
// named as an existing collection (the create's result, E_INVALIDARG), the decoy without a mangled name and the decoy with both of closest's names (no unique name:
// E_INVALIDARG and no device error, never broadened),
// DispatchRays on a closed list and SetPipelineState1 with another device context's state object (one error on the
// list). The raytracing tier engine-ddi reports stays NOT_SUPPORTED (INTEGRATION.md).
namespace {
constexpr UINT32 kRecordValue = 0xB0253C09u;       // the hit group's local root constant: the word of a hit
constexpr UINT32 kRecordValue2 = 0xB0253C0Au;      // the same in the second pipeline's shader table
constexpr UINT32 kRecordValue3 = 0xB0253C0Bu;      // and in the third's
constexpr UINT32 kMixedValues[2] = {0xB0253C0Cu, 0xB0253C0Du};     // and in the two mixed-library pipelines'
constexpr UINT32 kImportValue = 0xB0253C0Eu;       // and in the collection-only executable's
constexpr UINT32 kGrowValue = 0xB0253C0Fu;         // and in the grown pipeline's
constexpr UINT32 kMissFar = 3;                      // what miss_far writes (fixture-raylib-b.hlsl)
constexpr UINT32 kTrapValue = 0xB0253CFFu;         // a word no shader should read: see the second and third tables
// The UPLOAD buffer: vertices at 0, the instance at kInstanceOffset, then the shader table, its records 32-byte and its
// tables 64-byte aligned (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
// the tables of the later pipelines kTable2 to kTable7 bytes after the first.
constexpr UINT64 kUploadBytes = 7168, kTable2 = 1024, kTable3 = 2048, kTable4 = 3072, kTable5 = 4096, kTable6 = 5120,
                 kTable7 = 6144;
constexpr UINT64 kRaygenOffset = 512, kMissOffset = 576, kHitOffset = 640;
constexpr UINT64 kRecordBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
constexpr UINT64 kHitStride = 64;                   // identifier, the constants, padding to the record alignment
constexpr UINT64 kTableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
static_assert(kRaygenOffset % kTableAlignment == 0 && kMissOffset % kTableAlignment == 0 &&
                  kHitOffset % kTableAlignment == 0 && kHitStride % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT == 0 &&
                  kRecordBytes + 2 * sizeof(UINT32) <= kHitStride && kMissOffset + kHitStride <= kHitOffset &&
                  kInstanceOffset + sizeof(D3D12_RAYTRACING_INSTANCE_DESC) <= kRaygenOffset &&
                  kHitOffset + kHitStride <= kTable2 && kTable2 % kTableAlignment == 0 &&
                  kTable3 - kTable2 == kTable2 && kTable4 - kTable3 == kTable2 && kTable5 - kTable4 == kTable2 &&
                  kTable6 - kTable5 == kTable2 && kTable7 - kTable6 == kTable2 &&
                  kTable7 + kHitOffset + kHitStride <= kUploadBytes,
              "shader table layout");
constexpr UINT64 kPipelineOutBytes = 7 * 256;      // one 64-word result per pipeline, 256 bytes apart
// The indirect dispatches (9): a command signature of one DISPATCH_RAYS argument, its stride above the record's 104
// bytes. Their UPLOAD buffer: the two records kRaysStride apart, the count words at kCountsOffset (the first
// kIndirectArgsBytes bytes, which the list also copies into a DEFAULT buffer), and the output's prefill at
// kPrefillOffset. In the output, one 64-word result per variant and argument buffer, 256 bytes apart, the UPLOAD
// buffer's first.
constexpr UINT kRaysStride = 128;
constexpr UINT32 kCounts[] = {1, 2, 0, 3};          // the count words; 3 is above every maximum count here
constexpr UINT64 kCountsOffset = 2 * UINT64{kRaysStride}, kIndirectArgsBytes = kCountsOffset + sizeof(kCounts);
constexpr UINT kIndirectVariants = 6, kIndirectSections = 2 * kIndirectVariants;
constexpr UINT64 kIndirectOutBytes = kIndirectSections * 256, kPrefillOffset = 512,
                 kIndirectUploadBytes = kPrefillOffset + kIndirectOutBytes;
constexpr UINT32 kPrefill = 0xB0253CEEu;            // what a word no ray wrote holds
constexpr UINT kNoCount = UINT_MAX;
// A variant: the maximum count, the index of the count word it names in kCounts (kNoCount: no count buffer), the
// rows its result holds when every record up to the smaller of the count and the maximum is traced (the first record
// is 8x4, the second 8x8), and the rows an engine that traces the first record alone and nothing with a count buffer
// writes (vkd3d-proton upstream).
struct IndirectVariant {
    UINT max_count;
    UINT count;
    UINT rows;
    UINT rows_first_only;
};
constexpr IndirectVariant kIndirect[kIndirectVariants] = {
    {1, kNoCount, kGrid / 2, kGrid / 2},    // max 1, no count buffer
    {2, 0, kGrid / 2, 0},                   // max 2, count 1
    {2, 1, kGrid, 0},                       // max 2, count 2
    {2, kNoCount, kGrid, kGrid / 2},        // max 2, no count buffer
    {2, 2, 0, 0},                           // max 2, count 0: nothing
    {2, 3, kGrid, 0},                       // max 2, count 3: both records
};
static_assert(sizeof(D3D12_DISPATCH_RAYS_DESC) == 104 && kRaysStride > sizeof(D3D12_DISPATCH_RAYS_DESC) &&
                  kRaysStride % sizeof(UINT32) == 0 && kIndirectArgsBytes <= kPrefillOffset && kPrefill != kMiss &&
                  kPrefill != kRecordValue,
              "indirect dispatch layout");

// The 64 words of an indirect result whose first rows rows the first pipeline wrote, the rest at the prefill.
void rows_written(const UINT32* expected, UINT rows, UINT32* out) {
    for (UINT i = 0; i < kWords; ++i) out[i] = i < rows * kGrid ? expected[i] : kPrefill;
}
constexpr LPCWSTR kRaygenMangled = L"\x01?raygen@@YAXXZ";
constexpr LPCWSTR kMissMangled = L"\x01?miss@@YAXUPayload@@@Z";
constexpr LPCWSTR kMissFarMangled = L"\x01?miss_far@@YAXUPayload@@@Z";
constexpr LPCWSTR kClosestMangled = L"\x01?closest@@YAXUPayload@@UBuiltInTriangleIntersectionAttributes@@@Z";

void* create_root_signature(Env& env, Device& device, const D3D12DDI_ROOT_SIGNATURE_0013& rs, HRESULT* hr) {
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{};
    args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
    args.pRootSignature_1_1 = &rs;
    void* storage = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &args));
    *hr = storage ? env.core.pfnCreateRootSignature(device.h(), &args, D3D12DDI_HROOTSIGNATURE{storage}) : E_OUTOFMEMORY;
    return storage;
}

// The pipeline's global root signature SRV(t0) UAV(u0) and one 32-bit constant in b0 space2 (parameter 2, miss's
// value), and its local one, one 32-bit constant in b0 space1; the decoy's local root signature, a constant in b0
// space3 and then one in b0 space1, so that closest would read the record's next word under it; the trap's, a
// constant in b0 space1 and one in b0 space4, a register no global parameter has (Raytracing.md:966-968: the local
// and global root signatures of a shader do not overlap).
struct RootSignatures {
    void* global = nullptr;
    void* local = nullptr;
    void* decoy = nullptr;
    void* trap = nullptr;
    HRESULT hr = E_FAIL;
};
constexpr UINT kMissParameter = 2;
RootSignatures create_pipeline_root_signatures(Env& env, Device& device) {
    RootSignatures r;
    D3D12DDI_ROOT_PARAMETER_0013 params[3]{};
    params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    params[0].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    params[1].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    params[kMissParameter].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kMissParameter].Constants = {0, 2, 1};
    params[kMissParameter].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    const D3D12DDI_ROOT_SIGNATURE_0013 global{3, params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDI_ROOT_PARAMETER_0013 constant{};
    constant.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant.Constants = {0, 1, 1};
    constant.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    const D3D12DDI_ROOT_SIGNATURE_0013 local{1, &constant, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
    D3D12DDI_ROOT_PARAMETER_0013 decoy_params[2] = {constant, constant};
    decoy_params[0].Constants.RegisterSpace = 3;
    const D3D12DDI_ROOT_SIGNATURE_0013 decoy{2, decoy_params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
    D3D12DDI_ROOT_PARAMETER_0013 trap_params[2] = {constant, constant};
    trap_params[1].Constants.RegisterSpace = 4;
    const D3D12DDI_ROOT_SIGNATURE_0013 trap{2, trap_params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
    HRESULT hr_local = E_FAIL, hr_decoy = E_FAIL, hr_trap = E_FAIL;
    r.global = create_root_signature(env, device, global, &r.hr);
    r.local = create_root_signature(env, device, local, &hr_local);
    r.decoy = create_root_signature(env, device, decoy, &hr_decoy);
    r.trap = create_root_signature(env, device, trap, &hr_trap);
    if (r.hr == S_OK) r.hr = hr_local;
    if (r.hr == S_OK) r.hr = hr_decoy;
    if (r.hr == S_OK) r.hr = hr_trap;
    return r;
}

void destroy_root_signatures(Env& env, Device& device, const RootSignatures& r) {
    for (void* storage : {r.global, r.local, r.decoy, r.trap})
        if (storage) env.core.pfnDestroyRootSignature(device.h(), D3D12DDI_HROOTSIGNATURE{storage});
}

// The pipeline in the DDI form, as this harness models the runtime handing it over. Documented (Raytracing.md:9490-9498):
// library subobjects arrive as plain DDI subobjects, associations as an explicit list per export. The exact shape
// below is INFERENCE until a lab run logs a real description (INTEGRATION.md "Ray tracing state objects"): DDI
// subobjects in the application's order, root
// signatures by handle, the library without its size, the hit group with its SummaryFlags, no association subobject,
// and last a SHADER_EXPORT_SUMMARY that names every shader export with the subobjects associated with it (the global
// root signature and both configurations for each; the local root signature for closest, the hit group's shader). The
// mangled names are dxc's for fixture-raylib.hlsl. Holds pointers into itself: describe fills it in place.
struct PipelineDesc {
    D3D12DDI_STATE_OBJECT_CONFIG_0054 config;
    D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054 global;
    D3D12DDI_LOCAL_ROOT_SIGNATURE_0054 local;
    D3D12DDI_EXPORT_DESC_0054 exports[3];
    D3D12DDI_DXIL_LIBRARY_DESC_0054 library;
    D3D12DDI_DXIL_LIBRARY_DESC_0054 library_b;
    D3D12DDI_RAYTRACING_SHADER_CONFIG_0054 shader_config;
    D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075 pipeline_config;
    D3D12DDI_HIT_GROUP_DESC_0054 hit_group;
    D3D12DDI_LOCAL_ROOT_SIGNATURE_0054 decoy_local;
    D3D12DDI_STATE_SUBOBJECT_0054 subobjects[9];    // the summary is the eighth; the ninth is room for one more
    const D3D12DDI_STATE_SUBOBJECT_0054* common[3]; // global root signature, shader and pipeline configuration
    const D3D12DDI_STATE_SUBOBJECT_0054* hit[4];    // the same and the local root signature
    const D3D12DDI_STATE_SUBOBJECT_0054* decoy[4];  // the same with the decoy's local root signature
    D3D12DDI_FUNCTION_SUMMARY_NODE_0054 nodes[4];   // raygen, miss, closest; the decoy or miss_far
    D3D12DDI_FUNCTION_SUMMARY_0054 summary;
    D3D12DDIARG_CREATE_STATE_OBJECT_0054 args;
};
constexpr UINT kDescribed = 8;

// named_exports: the library lists its three exports; otherwise NumExports 0, every export of the library.
void describe(PipelineDesc& d, void* global, void* local, const UINT* library, bool named_exports) {
    d.config = {D3D12DDI_STATE_OBJECT_FLAG_NONE};
    d.global = {D3D12DDI_HROOTSIGNATURE{global}};
    d.local = {D3D12DDI_HROOTSIGNATURE{local}};
    d.exports[0] = {L"raygen", nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    d.exports[1] = {L"miss", nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    d.exports[2] = {L"closest", nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    d.library = {library, named_exports ? 3u : 0u, named_exports ? d.exports : nullptr};
    d.shader_config = {sizeof(UINT32), 2 * sizeof(float)};     // the payload, the triangle's barycentrics
    d.pipeline_config = {1, D3D12DDI_RAYTRACING_PIPELINE_FLAG_NONE};
    d.hit_group = {L"hitgroup", D3D12DDI_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"closest", nullptr,
                   D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.subobjects[0] = {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &d.config};
    d.subobjects[1] = {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &d.global};
    d.subobjects[2] = {D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &d.local};
    d.subobjects[3] = {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &d.library};
    d.subobjects[4] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &d.shader_config};
    d.subobjects[5] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &d.pipeline_config};
    d.subobjects[6] = {D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP, &d.hit_group};
    d.subobjects[7] = {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &d.summary};
    d.subobjects[8] = {};
    d.common[0] = d.hit[0] = &d.subobjects[1];
    d.common[1] = d.hit[1] = &d.subobjects[4];
    d.common[2] = d.hit[2] = &d.subobjects[5];
    d.hit[3] = &d.subobjects[2];
    d.nodes[0] = {L"raygen", kRaygenMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.nodes[1] = {L"miss", kMissMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.nodes[2] = {L"closest", kClosestMangled, 4, d.hit, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.summary = {3, d.nodes, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.args = {D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, kDescribed, d.subobjects};
}

// Adds a fourth summary export that shares the plain name "closest" with the real one and is associated with the
// decoy's local root signature (the ninth subobject), with the given mangled name (null for none): kDecoyMangled, which
// no export of the library carries, or the real closest's.
constexpr LPCWSTR kDecoyMangled = L"\x01?closest@@YAXUDecoy@@@Z";
void add_decoy(PipelineDesc& d, void* decoy_local, LPCWSTR mangled) {
    d.decoy_local = {D3D12DDI_HROOTSIGNATURE{decoy_local}};
    d.subobjects[kDescribed] = {D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &d.decoy_local};
    d.decoy[0] = d.common[0];
    d.decoy[1] = d.common[1];
    d.decoy[2] = d.common[2];
    d.decoy[3] = &d.subobjects[kDescribed];
    d.nodes[3] = {L"closest", mangled, 4, d.decoy,
                  D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.summary.NumExportedFunctions = 4;
    d.args.NumSubobjects = kDescribed + 1;
}

// Mixed libraries, on a description with named exports: the first library lists closest by its mangled name alone,
// without a rename (Raytracing.md:3497), and the hit group imports it by that name; the ninth subobject is a second
// library with no export list (fixture-raylib-b.h, every export: miss_far), whose summary export has the global root
// signature and both configurations. b_first puts the second library ahead of the first in the description.
void add_export_all_library(PipelineDesc& d, const UINT* library_b, bool b_first) {
    d.exports[2] = {kClosestMangled, nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    d.hit_group.ClosestHitShaderImport = kClosestMangled;
    d.library_b = {library_b, 0, nullptr};
    d.subobjects[kDescribed] = {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &d.library_b};
    if (b_first) std::swap(d.subobjects[3], d.subobjects[kDescribed]);
    d.nodes[3] = {L"miss_far", kMissFarMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.summary.NumExportedFunctions = 4;
    d.args.NumSubobjects = kDescribed + 1;
}

// A collection-only executable in the DDI form: its own configuration, the global root signature, the collection
// imported whole (NumExports 0), both configurations and no DXIL library; the summary names raygen, miss and closest
// with the executable's own global root signature and configurations. The collection's local root association for
// closest is not restated: how the runtime passes an inherited association is not measured (INTEGRATION.md).
struct ImportDesc {
    D3D12DDI_STATE_OBJECT_CONFIG_0054 config;
    D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054 global;
    D3D12DDI_EXISTING_COLLECTION_DESC_0054 collection;
    D3D12DDI_RAYTRACING_SHADER_CONFIG_0054 shader_config;
    D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075 pipeline_config;
    D3D12DDI_STATE_SUBOBJECT_0054 subobjects[6];    // the summary is the sixth
    const D3D12DDI_STATE_SUBOBJECT_0054* common[3];
    D3D12DDI_FUNCTION_SUMMARY_NODE_0054 nodes[3];
    D3D12DDI_FUNCTION_SUMMARY_0054 summary;
    D3D12DDIARG_CREATE_STATE_OBJECT_0054 args;
};
void describe_import(ImportDesc& d, void* global, void* collection) {
    d.config = {D3D12DDI_STATE_OBJECT_FLAG_NONE};
    d.global = {D3D12DDI_HROOTSIGNATURE{global}};
    d.collection = {D3D12DDI_HSTATEOBJECT_0054{collection}, 0, nullptr};
    d.shader_config = {sizeof(UINT32), 2 * sizeof(float)};
    d.pipeline_config = {1, D3D12DDI_RAYTRACING_PIPELINE_FLAG_NONE};
    d.subobjects[0] = {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &d.config};
    d.subobjects[1] = {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &d.global};
    d.subobjects[2] = {D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &d.collection};
    d.subobjects[3] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &d.shader_config};
    d.subobjects[4] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &d.pipeline_config};
    d.subobjects[5] = {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &d.summary};
    d.common[0] = &d.subobjects[1];
    d.common[1] = &d.subobjects[3];
    d.common[2] = &d.subobjects[4];
    d.nodes[0] = {L"raygen", kRaygenMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.nodes[1] = {L"miss", kMissMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.nodes[2] = {L"closest", kClosestMangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.summary = {3, d.nodes, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.args = {D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 6, d.subobjects};
}

// An addition in the DDI form, a description valid on its own (Raytracing.md:3781-3783): a configuration allowing
// additions, the global root signature, one library listing one export, both configurations, and a summary naming that
// export with the addition's global root signature and configurations; parent is the state object grown from.
struct AdditionDesc {
    D3D12DDI_STATE_OBJECT_CONFIG_0054 config;
    D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054 global;
    D3D12DDI_EXPORT_DESC_0054 export_desc;
    D3D12DDI_DXIL_LIBRARY_DESC_0054 library;
    D3D12DDI_RAYTRACING_SHADER_CONFIG_0054 shader_config;
    D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075 pipeline_config;
    D3D12DDI_STATE_SUBOBJECT_0054 subobjects[6];    // the summary is the sixth
    const D3D12DDI_STATE_SUBOBJECT_0054* common[3];
    D3D12DDI_FUNCTION_SUMMARY_NODE_0054 node;
    D3D12DDI_FUNCTION_SUMMARY_0054 summary;
    D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 args;
};
void describe_addition(AdditionDesc& d, void* global, const UINT* library, LPCWSTR name, LPCWSTR mangled,
                       void* parent) {
    d.config = {D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    d.global = {D3D12DDI_HROOTSIGNATURE{global}};
    d.export_desc = {name, nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    d.library = {library, 1, &d.export_desc};
    d.shader_config = {sizeof(UINT32), 2 * sizeof(float)};
    d.pipeline_config = {1, D3D12DDI_RAYTRACING_PIPELINE_FLAG_NONE};
    d.subobjects[0] = {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &d.config};
    d.subobjects[1] = {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &d.global};
    d.subobjects[2] = {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &d.library};
    d.subobjects[3] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &d.shader_config};
    d.subobjects[4] = {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &d.pipeline_config};
    d.subobjects[5] = {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &d.summary};
    d.common[0] = &d.subobjects[1];
    d.common[1] = &d.subobjects[3];
    d.common[2] = &d.subobjects[4];
    d.node = {name, mangled, 3, d.common, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.summary = {1, &d.node, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    d.args = {D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 6, d.subobjects, D3D12DDI_HSTATEOBJECT_0054{parent}};
}

// The public reference count of a record's engine object, read by an AddRef and Release pair. The engine keeps its
// own internal count for what one state object holds of another (raytracing_pipeline.c:101-121, 2762-2769), so this
// one counts engine-ddi's references.
ULONG public_references(void* storage) {
    IUnknown* object = storage ? engine_ddi::harness_engine_object(storage) : nullptr;
    if (!object) return 0;
    const ULONG count = object->AddRef();
    object->Release();
    return count - 1;
}

// A grown state object in fresh private storage; the AddToStateObject result.
HRESULT add_to_state_object(Env& env, Device& device, const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072& args,
                            void** storage, int* rt) {
    *storage = env.storage.alloc(env.core.pfnCalcPrivateAddToStateObjectSize(device.h(), &args));
    if (!*storage) return E_OUTOFMEMORY;
    return env.core.pfnAddToStateObject(device.h(), &args, D3D12DDI_HSTATEOBJECT_0054{*storage},
                                        D3D12DDI_HRTSTATEOBJECT_0054{rt});
}

// A state object in fresh private storage; the create's result.
HRESULT create_state_object(Env& env, Device& device, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args, void** storage,
                            int* rt) {
    *storage = env.storage.alloc(env.core.pfnCalcPrivateStateObjectSize(device.h(), &args));
    if (!*storage) return E_OUTOFMEMORY;
    return env.core.pfnCreateStateObject(device.h(), &args, D3D12DDI_HSTATEOBJECT_0054{*storage},
                                         D3D12DDI_HRTSTATEOBJECT_0054{rt});
}

// The API description of the last create that reached the engine, copied by the state object observer
// (harness_set_state_object_observer) immediately before the engine's CreateStateObject.
struct Captured {
    struct Association {
        D3D12_STATE_SUBOBJECT_TYPE type;        // of the associated subobject
        const void* root;                       // its root signature, for a global or local root signature
        std::vector<std::wstring> names;        // none: an explicit default
    };
    struct Import {
        const void* collection;                 // the engine object
        UINT exports;                           // NumExports
    };
    UINT calls = 0;
    bool complete = false;                      // the copy did not run out of memory
    D3D12_STATE_OBJECT_TYPE type{};
    const void* parent = nullptr;               // the engine object an addition grows from
    std::vector<const void*> locals;            // the declared local root signatures, in order
    std::vector<UINT> library_exports;          // NumExports of each DXIL library, in order
    std::vector<Import> imports;                // each EXISTING_COLLECTION, in order
    std::vector<Association> associations;
};

const void* root_of(const D3D12_STATE_SUBOBJECT& s) noexcept {
    if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE)
        return static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(s.pDesc)->pLocalRootSignature;
    if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE)
        return static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(s.pDesc)->pGlobalRootSignature;
    return nullptr;
}

void capture_state_object(const D3D12_STATE_OBJECT_DESC& desc, ID3D12StateObject* parent, void* user) {
    auto& c = *static_cast<Captured*>(user);
    const UINT calls = c.calls + 1;
    c = Captured{};
    c.calls = calls;
    c.type = desc.Type;
    c.parent = parent;
    try {
        for (UINT i = 0; i < desc.NumSubobjects; ++i) {
            const D3D12_STATE_SUBOBJECT& s = desc.pSubobjects[i];
            if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE) c.locals.push_back(root_of(s));
            if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY)
                c.library_exports.push_back(static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc)->NumExports);
            if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
                const auto& import = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
                c.imports.push_back({import.pExistingCollection, import.NumExports});
            }
            if (s.Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) continue;
            const auto& a = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s.pDesc);
            Captured::Association copy{a.pSubobjectToAssociate->Type, root_of(*a.pSubobjectToAssociate), {}};
            for (UINT e = 0; e < a.NumExports; ++e) copy.names.emplace_back(a.pExports[e]);
            c.associations.push_back(std::move(copy));
        }
        c.complete = true;
    } catch (const std::bad_alloc&) {
    }
}

// Installs capture_state_object for its lifetime.
struct CaptureScope {
    Captured captured;
    CaptureScope() noexcept { engine_ddi::harness_set_state_object_observer(capture_state_object, &captured); }
    ~CaptureScope() { engine_ddi::harness_set_state_object_observer(nullptr, nullptr); }
    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;
};

// The engine root signature of a root signature record, as a translated description names it.
const void* engine_root(void* storage) noexcept {
    return static_cast<ID3D12RootSignature*>(engine_ddi::harness_engine_object(storage));
}

// The names the associations of c with a subobject of type (and root, unless null) hand to the engine, in order.
std::vector<std::wstring> associated(const Captured& c, D3D12_STATE_SUBOBJECT_TYPE type, const void* root) {
    std::vector<std::wstring> names;
    for (const Captured::Association& a : c.associations)
        if (a.type == type && (!root || a.root == root)) names.insert(names.end(), a.names.begin(), a.names.end());
    return names;
}

bool names_are(const std::vector<std::wstring>& names, std::initializer_list<LPCWSTR> expected) {
    if (names.size() != expected.size()) return false;
    size_t i = 0;
    for (LPCWSTR name : expected)
        if (names[i++] != name) return false;
    return true;
}

// The local root signatures an explicit default (an association with no export) hands to the engine.
std::vector<const void*> local_defaults(const Captured& c) {
    std::vector<const void*> roots;
    for (const Captured::Association& a : c.associations)
        if (a.type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && a.names.empty()) roots.push_back(a.root);
    return roots;
}

// An export by any of names in a local root signature's association.
bool locally_associated(const Captured& c, std::initializer_list<LPCWSTR> names) {
    const std::vector<std::wstring> all = associated(c, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, nullptr);
    for (LPCWSTR name : names)
        if (std::find(all.begin(), all.end(), name) != all.end()) return true;
    return false;
}
} // namespace

void test_raytracing_pipeline(Env& env) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    const HRESULT hr_options = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (hr_options != S_OK || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        std::printf("SKIP  raytracing pipeline: the engine reports raytracing tier %d on this GPU (hr %08lx)\n",
                    static_cast<int>(options5.RaytracingTier), static_cast<unsigned long>(hr_options));
        return;
    }
    UINT32 expected[kWords];
    expected_words(expected);
    UINT hits = 0;
    for (UINT32& w : expected) {
        hits += w == kHit ? 1u : 0u;
        w = w == kHit ? kRecordValue : kMiss;
    }

    StubMemory m;
    check(load_stub(env, m), "raytracing pipeline: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "raytracing pipeline: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[0];

    // The scene of round trip 8: one triangle, one instance.
    Buffer upload;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, kUploadBytes, false, upload);
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
    Buffer blas, tlas, scratch, out, readback;
    const HRESULT hr_buffers[] = {
        hr_u,
        create_structure_buffer(env, device, blas_info.ResultDataMaxSizeInBytes, blas),
        create_structure_buffer(env, device, tlas_info.ResultDataMaxSizeInBytes, tlas),
        create_buffer(env, device, HeapKind::Default,
                      std::max(blas_info.ScratchDataSizeInBytes, tlas_info.ScratchDataSizeInBytes), true, scratch),
        create_buffer(env, device, HeapKind::Default, kPipelineOutBytes, true, out),
        create_buffer(env, device, HeapKind::Readback, kPipelineOutBytes, false, readback),
    };
    bool created = upload_va != 0;
    for (HRESULT h : hr_buffers) created = created && h == S_OK;
    D3D12DDI_GPU_VIRTUAL_ADDRESS va[4]{};
    const Buffer* named[4] = {&blas, &tlas, &scratch, &out};
    for (int i = 0; created && i < 4; ++i) {
        va[i] = env.core.pfnCheckResourceVirtualAddress(device.h(), named[i]->hres());
        created = va[i] != 0 && va[i] % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT == 0;
    }
    checkf(created && upload_va % kTableAlignment == 0,
           "raytracing pipeline: the scene's UPLOAD, structure, scratch, output and READBACK buffers");
    if (!created) return;
    const D3D12DDI_GPU_VIRTUAL_ADDRESS blas_va = va[0], tlas_va = va[1], scratch_va = va[2], out_va = va[3];

    // 1. The state object.
    const RootSignatures rs = create_pipeline_root_signatures(env, device);
    DdiShader library;
    const bool stripped = ddi_form(g_fixture_raylib, sizeof(g_fixture_raylib), library);
    PipelineDesc desc;
    describe(desc, rs.global, rs.local, library.code.data(), true);
    CaptureScope capture;
    void* so_storage = nullptr;
    int so_rt = 0;
    const HRESULT hr_so = rs.hr == S_OK && stripped ? create_state_object(env, device, desc.args, &so_storage, &so_rt)
                                                    : E_FAIL;
    const D3D12DDI_HSTATEOBJECT_0054 hso{so_storage};
    checkf(hr_so == S_OK && !device.shell.device_errors,
           "raytracing pipeline: CreateStateObject from the DDI form: global and local root signature by handle, the "
           "dxc lib_6_3 library's DXIL part (%zu DWORDs, three named exports), shader and pipeline configuration, a "
           "triangles hit group, a summary of three exports and no association (hr %08lx %08lx)",
           library.code.size(), static_cast<unsigned long>(rs.hr), static_cast<unsigned long>(hr_so));
    // The description the engine received (5): closest's local root signature by the name the library lists, and the
    // empty local root signature, declared last, as the explicit default that raygen and miss, associated with no
    // local root signature, fall to.
    const void* const local_root = engine_root(rs.local);
    const void* const rs_global_root = engine_root(rs.global);
    const void* const trap_root = engine_root(rs.trap);
    const void* const decoy_root = engine_root(rs.decoy);
    const Captured seen1 = capture.captured;
    const std::vector<const void*> defaults = local_defaults(seen1);
    const void* const empty_local = defaults.size() == 1 ? defaults[0] : nullptr;
    const bool first_ok = seen1.complete && seen1.calls == 1 && local_root && empty_local &&
                          empty_local != local_root && empty_local != trap_root && empty_local != decoy_root &&
                          seen1.locals.size() == 2 && seen1.locals[0] == local_root && seen1.locals[1] == empty_local &&
                          names_are(associated(seen1, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, local_root),
                                    {L"closest"}) &&
                          !locally_associated(seen1, {L"raygen", kRaygenMangled, L"miss", kMissMangled});
    checkf(first_ok,
           "raytracing pipeline: the description handed to the engine: closest's local root signature associated with "
           "\"closest\", as the library lists it; raygen and miss in no local root association; one more local root "
           "signature, declared last, with an association of no export, the explicit default (%u creates seen, %zu "
           "local root signatures, %zu explicit defaults)",
           seen1.calls, seen1.locals.size(), defaults.size());

    // 2. Identifiers and stack sizes.
    const wchar_t* const id_names[3] = {L"raygen", L"miss", L"hitgroup"};
    const void* ids[3]{};
    bool ids_ok = hr_so == S_OK;
    for (int i = 0; ids_ok && i < 3; ++i) {
        ids[i] = env.core.pfnGetShaderIdentifier(hso, id_names[i]);
        static const BYTE zero[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]{};
        ids_ok = ids[i] && std::memcmp(ids[i], zero, sizeof(zero)) != 0;
    }
    ids_ok = ids_ok && std::memcmp(ids[0], ids[1], kRecordBytes) && std::memcmp(ids[0], ids[2], kRecordBytes) &&
             std::memcmp(ids[1], ids[2], kRecordBytes);
    checkf(ids_ok,
           "raytracing pipeline: GetShaderIdentifier of raygen, miss and hitgroup: %u bytes each, not all zero, all "
           "three different",
           D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    const UINT stack_raygen = hr_so == S_OK ? env.core.pfnGetShaderStackSize(hso, L"raygen") : UINT_MAX;
    const UINT stack_miss = hr_so == S_OK ? env.core.pfnGetShaderStackSize(hso, L"miss") : UINT_MAX;
    const UINT stack_closest = hr_so == S_OK ? env.core.pfnGetShaderStackSize(hso, L"hitgroup::closesthit") : UINT_MAX;
    const UINT stack_unknown = hr_so == S_OK ? env.core.pfnGetShaderStackSize(hso, L"nosuchexport") : 0;
    const UINT pipeline_stack = hr_so == S_OK ? env.core.pfnGetPipelineStackSize(hso) : 0;
    if (hr_so == S_OK) env.core.pfnSetPipelineStackSize(hso, pipeline_stack + 64);   // more is always enough
    const UINT pipeline_stack_set = hr_so == S_OK ? env.core.pfnGetPipelineStackSize(hso) : 0;
    checkf(stack_raygen != UINT_MAX && stack_miss != UINT_MAX && stack_closest != UINT_MAX && stack_unknown == UINT_MAX &&
               pipeline_stack_set == pipeline_stack + 64 && !device.shell.device_errors,
           "raytracing pipeline: stack sizes raygen %u, miss %u, hitgroup::closesthit %u bytes, UINT_MAX for an "
           "unknown export; pipeline stack size %u, set to %u and read back as %u",
           stack_raygen, stack_miss, stack_closest, pipeline_stack, pipeline_stack + 64, pipeline_stack_set);
    if (hr_so != S_OK || !ids_ok) return;

    // 4, the state object: dxc's whole container, every export, and the decoy with a mangled name of its own.
    std::vector<UINT> container((sizeof(g_fixture_raylib) + sizeof(UINT) - 1) / sizeof(UINT), 0u);
    std::memcpy(container.data(), g_fixture_raylib, sizeof(g_fixture_raylib));
    PipelineDesc whole;
    describe(whole, rs.global, rs.local, container.data(), false);
    add_decoy(whole, rs.decoy, kDecoyMangled);
    void* so2_storage = nullptr;
    int so2_rt = 0;
    const HRESULT hr_so2 = create_state_object(env, device, whole.args, &so2_storage, &so2_rt);
    const D3D12DDI_HSTATEOBJECT_0054 hso2{so2_storage};
    const void* ids2[3]{};
    bool ids2_ok = hr_so2 == S_OK;
    for (int i = 0; ids2_ok && i < 3; ++i) {
        ids2[i] = env.core.pfnGetShaderIdentifier(hso2, id_names[i]);
        ids2_ok = ids2[i] != nullptr;
    }
    checkf(ids2_ok && !device.shell.device_errors,
           "raytracing pipeline: the library as the whole dxc container (%zu DWORDs, NumExports 0) with a decoy export "
           "sharing the plain name \"closest\" under a mangled name of its own and a second local root signature: "
           "CreateStateObject and three identifiers (hr %08lx)",
           container.size(), static_cast<unsigned long>(hr_so2));
    if (!ids2_ok) return;
    // The names handed to the engine: closest's local root signature names it by its mangled name alone, the decoy's
    // names the decoy's mangled name, and no local root association names the plain "closest" both share.
    const Captured& seen2 = capture.captured;
    checkf(seen2.complete &&
               names_are(associated(seen2, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, local_root),
                         {kClosestMangled}) &&
               names_are(associated(seen2, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, decoy_root),
                         {kDecoyMangled}) &&
               !locally_associated(seen2, {L"closest"}) && local_defaults(seen2) == defaults,
           "raytracing pipeline: the description of the decoy pipeline handed to the engine: closest's local root "
           "signature associated with closest's mangled name, the decoy's with the decoy's mangled name, neither with "
           "the plain \"closest\"; raygen and miss fall to the same empty explicit default");

    // 5, the state object: closest with the trap's local root signature, raygen and miss with none.
    PipelineDesc trapped;
    describe(trapped, rs.global, rs.trap, library.code.data(), true);
    void* so3_storage = nullptr;
    int so3_rt = 0;
    const HRESULT hr_so3 = create_state_object(env, device, trapped.args, &so3_storage, &so3_rt);
    const D3D12DDI_HSTATEOBJECT_0054 hso3{so3_storage};
    const void* ids3[3]{};
    bool ids3_ok = hr_so3 == S_OK;
    for (int i = 0; ids3_ok && i < 3; ++i) {
        ids3[i] = env.core.pfnGetShaderIdentifier(hso3, id_names[i]);
        ids3_ok = ids3[i] != nullptr;
    }
    checkf(ids3_ok && !device.shell.device_errors,
           "raytracing pipeline: a local root signature associated with closest alone, raygen and miss associated "
           "with none: CreateStateObject and three identifiers (hr %08lx)",
           static_cast<unsigned long>(hr_so3));
    if (!ids3_ok) return;
    // The absent association stays absent in what the engine receives: the trap is associated with "closest" alone,
    // raygen and miss with no local root signature, and the context's one empty local root signature (the object
    // of the first create) is declared last with an association of no export, the explicit default. Without it the
    // engine's declared default, the trap, would reach raygen and miss (INTEGRATION.md).
    const Captured& seen3 = capture.captured;
    checkf(seen3.complete && seen3.locals.size() == 2 && seen3.locals[0] == trap_root && seen3.locals[1] == empty_local &&
               local_defaults(seen3) == defaults &&
               names_are(associated(seen3, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, trap_root), {L"closest"}) &&
               !locally_associated(seen3, {L"raygen", kRaygenMangled, L"miss", kMissMangled}),
           "raytracing pipeline: the description of the trap pipeline handed to the engine: the trap associated with "
           "\"closest\" alone, raygen and miss in no local root association, and the empty local root signature of the "
           "first create declared last as the explicit default (%zu local root signatures, %zu explicit defaults)",
           seen3.locals.size(), local_defaults(seen3).size());

    // 6, the state objects: a library listing closest by its mangled name beside one exporting everything, in both
    // orders. The engine knows a listed export by the listed name alone, with no mangled name (dxil.c, the NumExports
    // branch), so the association must name closest by that mangled name; by its plain name it would reach no export.
    DdiShader library_b;
    const bool stripped_b = ddi_form(g_fixture_raylib_b, sizeof(g_fixture_raylib_b), library_b);
    PipelineDesc mixed[2];
    void* mixed_storage[2]{};
    const void* mixed_ids[2][3]{};
    for (int mp = 0; mp < 2; ++mp) {
        describe(mixed[mp], rs.global, rs.local, library.code.data(), true);
        add_export_all_library(mixed[mp], library_b.code.data(), mp == 1);
        int mixed_rt = 0;
        const HRESULT hr_m =
            stripped_b ? create_state_object(env, device, mixed[mp].args, &mixed_storage[mp], &mixed_rt) : E_FAIL;
        bool ok = hr_m == S_OK;
        for (int i = 0; ok && i < 3; ++i) {
            mixed_ids[mp][i] = env.core.pfnGetShaderIdentifier(D3D12DDI_HSTATEOBJECT_0054{mixed_storage[mp]}, id_names[i]);
            ok = mixed_ids[mp][i] != nullptr;
        }
        checkf(ok && !device.shell.device_errors,
               "raytracing pipeline: a library listing closest by its mangled name alone %s a library with no export "
               "list (miss_far): CreateStateObject and three identifiers (hr %08lx)",
               mp ? "after" : "before", static_cast<unsigned long>(hr_m));
        // The description handed to the engine, the libraries in this order: the local root signature's association
        // names closest alone, by the listed mangled name; the global one names every export, the listed ones as
        // listed and miss_far by its plain name; raygen, miss and miss_far fall to the empty explicit default.
        const Captured& mixed_seen = capture.captured;
        const std::vector<UINT> order = mp ? std::vector<UINT>{0u, 3u} : std::vector<UINT>{3u, 0u};
        const std::vector<std::wstring> global_names =
            associated(mixed_seen, D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, nullptr);
        checkf(mixed_seen.complete && mixed_seen.library_exports == order &&
                   names_are(associated(mixed_seen, D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, local_root),
                             {kClosestMangled}) &&
                   names_are(global_names, {L"raygen", L"miss", kClosestMangled, L"miss_far"}) &&
                   local_defaults(mixed_seen) == defaults,
               "raytracing pipeline: the description of the mixed-library pipeline, the listing library %s, handed to "
               "the engine: libraries of %s exports; closest's local root signature associated with the listed "
               "mangled name alone, the global one with raygen, miss, closest's mangled name and miss_far (%zu names), "
               "the empty explicit default for the rest",
               mp ? "second" : "first", mp ? "0 and 3" : "3 and 0", global_names.size());
        if (!ok) return;
    }

    // 7, the collection and the collection-only executable that imports it whole. The collection's DDI object is
    // destroyed before the executable is used: the executable's record holds the collection's engine object.
    PipelineDesc gathered;
    describe(gathered, rs.global, rs.local, library.code.data(), true);
    gathered.args.Type = D3D12DDI_STATE_OBJECT_TYPE_COLLECTION;
    void* collection_storage = nullptr;
    int collection_rt = 0;
    const HRESULT hr_collection = create_state_object(env, device, gathered.args, &collection_storage, &collection_rt);
    const bool collection_seen = capture.captured.complete && capture.captured.type == D3D12_STATE_OBJECT_TYPE_COLLECTION;
    ImportDesc import;
    describe_import(import, rs.global, collection_storage);
    void* importer_storage = nullptr;
    int importer_rt = 0;
    const ULONG collection_references = public_references(collection_storage);
    const HRESULT hr_importer =
        hr_collection == S_OK ? create_state_object(env, device, import.args, &importer_storage, &importer_rt) : E_FAIL;
    const D3D12DDI_HSTATEOBJECT_0054 himporter{importer_storage};
    BYTE importer_ids[3][kRecordBytes]{};
    bool importer_ok = hr_importer == S_OK;
    for (int i = 0; importer_ok && i < 3; ++i) {
        const void* id = env.core.pfnGetShaderIdentifier(himporter, id_names[i]);
        importer_ok = id != nullptr;
        if (id) std::memcpy(importer_ids[i], id, kRecordBytes);
    }
    checkf(hr_collection == S_OK && collection_seen && importer_ok && !device.shell.device_errors,
           "raytracing pipeline: a COLLECTION of the library, its root signatures, configurations and hit group, then a "
           "collection-only executable (EXISTING_COLLECTION of all exports, no DXIL library): both creates and three "
           "identifiers from the executable (hr %08lx %08lx)",
           static_cast<unsigned long>(hr_collection), static_cast<unsigned long>(hr_importer));
    // The description handed to the engine: the collection's engine object imported whole, no library, the global
    // root signature's association naming the three exports by the names the collection exposed.
    const Captured& import_seen = capture.captured;
    const void* const collection_engine =
        collection_storage ? static_cast<ID3D12StateObject*>(engine_ddi::harness_engine_object(collection_storage))
                           : nullptr;
    checkf(import_seen.complete && import_seen.type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE &&
               !import_seen.parent && import_seen.library_exports.empty() && import_seen.imports.size() == 1 &&
               collection_engine && import_seen.imports[0].collection == collection_engine &&
               import_seen.imports[0].exports == 0 && import_seen.locals.empty() &&
               names_are(associated(import_seen, D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, rs_global_root),
                         {L"raygen", L"miss", L"closest"}),
           "raytracing pipeline: the description of the collection-only executable handed to the engine: the "
           "collection's engine object with NumExports 0, no DXIL library, no local root signature, the global root "
           "signature associated with raygen, miss and closest, the names the collection exposed (%zu imports)",
           import_seen.imports.size());
    if (!importer_ok) return;
    {
        // A restricted import list: valid input, temporarily unsupported (the pinned engine's deferred import loop,
        // INTEGRATION.md): E_NOTIMPL before the engine, and no device error.
        ImportDesc restricted;
        describe_import(restricted, rs.global, collection_storage);
        D3D12DDI_EXPORT_DESC_0054 only{L"raygen", nullptr, D3D12DDI_EXPORT_FLAG_NONE};
        restricted.collection.NumExports = 1;
        restricted.collection.pExports = &only;
        const UINT calls = capture.captured.calls;
        void* storage = nullptr;
        int rt = 0;
        const HRESULT hr_restricted = create_state_object(env, device, restricted.args, &storage, &rt);
        if (storage) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{storage});
        checkf(hr_restricted == E_NOTIMPL && capture.captured.calls == calls && !device.shell.device_errors,
               "raytracing pipeline: an import of the collection with an export list: E_NOTIMPL before the engine, "
               "temporarily unsupported (hr %08lx)",
               static_cast<unsigned long>(hr_restricted));
    }
    const ULONG collection_held = public_references(collection_storage);
    checkf(collection_references && collection_held == collection_references + 1,
           "raytracing pipeline: the executable's record holds one reference to the collection's engine object, the "
           "refused import none (public references %lu, then %lu)",
           collection_references, collection_held);
    env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{collection_storage});

    // 8, growth: a pipeline allowing additions, its pipeline stack size set, grown by miss_far; the parent's DDI object
    // destroyed before the child is used, the child's table holding the parent's identifiers and the new one.
    PipelineDesc grown_from;
    describe(grown_from, rs.global, rs.local, library.code.data(), true);
    grown_from.config.Flags = D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
    void* parent_storage = nullptr;
    int parent_rt = 0;
    const HRESULT hr_parent = create_state_object(env, device, grown_from.args, &parent_storage, &parent_rt);
    const D3D12DDI_HSTATEOBJECT_0054 hparent{parent_storage};
    BYTE parent_ids[3][kRecordBytes]{};
    bool parent_ok = hr_parent == S_OK;
    for (int i = 0; parent_ok && i < 3; ++i) {
        const void* id = env.core.pfnGetShaderIdentifier(hparent, id_names[i]);
        parent_ok = id != nullptr;
        if (id) std::memcpy(parent_ids[i], id, kRecordBytes);
    }
    // A setting no default gives: the engine's computed sizes are small (0 on this development PC, 403).
    const UINT parent_stack = parent_ok ? env.core.pfnGetPipelineStackSize(hparent) + 4096 : 0;
    if (parent_ok) env.core.pfnSetPipelineStackSize(hparent, parent_stack);
    checkf(parent_ok && env.core.pfnGetPipelineStackSize(hparent) == parent_stack && !device.shell.device_errors,
           "raytracing pipeline: a pipeline allowing additions, three identifiers, its pipeline stack size set to %u "
           "(hr %08lx)",
           parent_stack, static_cast<unsigned long>(hr_parent));
    if (!parent_ok) return;
    AdditionDesc addition;
    describe_addition(addition, rs.global, library_b.code.data(), L"miss_far", kMissFarMangled, parent_storage);
    void* child_storage = nullptr;
    int child_rt = 0;
    const ULONG parent_references = public_references(parent_storage);
    const HRESULT hr_child = add_to_state_object(env, device, addition.args, &child_storage, &child_rt);
    const D3D12DDI_HSTATEOBJECT_0054 hchild{child_storage};
    const Captured add_seen = capture.captured;
    const UINT child_stack = hr_child == S_OK ? env.core.pfnGetPipelineStackSize(hchild) : 0;   // before any set
    const void* const miss_far_id = hr_child == S_OK ? env.core.pfnGetShaderIdentifier(hchild, L"miss_far") : nullptr;
    const void* const child_raygen = hr_child == S_OK ? env.core.pfnGetShaderIdentifier(hchild, L"raygen") : nullptr;
    BYTE miss_far[kRecordBytes]{};
    if (miss_far_id) std::memcpy(miss_far, miss_far_id, kRecordBytes);
    checkf(hr_child == S_OK && miss_far_id && child_raygen && !std::memcmp(child_raygen, parent_ids[0], kRecordBytes) &&
               !device.shell.device_errors,
           "raytracing pipeline: AddToStateObject with fixture-raylib-b listing miss_far: S_OK, an identifier for "
           "miss_far, and the parent's raygen identifier unchanged in the child (hr %08lx)",
           static_cast<unsigned long>(hr_child));
    checkf(child_stack == parent_stack,
           "raytracing pipeline: the child's pipeline stack size before any set is the parent's setting, %u (read %u)",
           parent_stack, child_stack);
    const void* const parent_engine =
        static_cast<ID3D12StateObject*>(engine_ddi::harness_engine_object(parent_storage));
    checkf(add_seen.complete && add_seen.parent == parent_engine && add_seen.imports.empty() &&
               add_seen.library_exports == std::vector<UINT>{1u} && add_seen.locals.empty() &&
               names_are(associated(add_seen, D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, rs_global_root),
                         {L"miss_far"}),
           "raytracing pipeline: the description of the addition handed to the engine: grown from the parent's engine "
           "object, one library listing one export, no local root signature, the global root signature associated with "
           "miss_far alone");
    if (hr_child != S_OK || !miss_far_id) return;
    // Refused growth, each one E_INVALIDARG and no device error: from a pipeline created without
    // ALLOW_STATE_OBJECT_ADDITIONS (the engine's check), and an addition exporting miss, which the parent has (the
    // bridge's check, before the engine).
    const auto refused_growth = [&](const char* what, void* from, const UINT* code, LPCWSTR name, LPCWSTR mangled,
                                    bool reaches_engine) {
        AdditionDesc bad;
        describe_addition(bad, rs.global, code, name, mangled, from);
        const UINT calls = capture.captured.calls;
        const uint32_t before = device.shell.device_errors;
        void* storage = nullptr;
        int rt = 0;
        const HRESULT hr_bad = add_to_state_object(env, device, bad.args, &storage, &rt);
        if (storage) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{storage});
        const UINT engine_calls = capture.captured.calls - calls;
        checkf(hr_bad == E_INVALIDARG && device.shell.device_errors == before && engine_calls == (reaches_engine ? 1u : 0u),
               "raytracing pipeline: AddToStateObject %s: 80070057 %s, no device error (hr %08lx, %u engine calls)",
               what, reaches_engine ? "from the engine" : "before the engine", static_cast<unsigned long>(hr_bad),
               engine_calls);
    };
    refused_growth("from a pipeline without ALLOW_STATE_OBJECT_ADDITIONS", so_storage, library_b.code.data(),
                   L"miss_far", kMissFarMangled, true);
    refused_growth("with an export named miss, as the parent's", parent_storage, library.code.data(), L"miss",
                   kMissMangled, false);
    const ULONG parent_held = public_references(parent_storage);
    checkf(parent_references && parent_held == parent_references + 1,
           "raytracing pipeline: the child's record holds one reference to the parent's engine object, the refused "
           "growth none (public references %lu, then %lu)",
           parent_references, parent_held);
    env.core.pfnDestroyStateObject(device.h(), hparent);

    // 3. Vertices, the instance and the shader table, then the list.
    void* cpu = nullptr;
    hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "raytracing pipeline: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK || !cpu) return;
    auto* bytes = static_cast<BYTE*>(cpu);
    std::memcpy(bytes, kTriangle, sizeof(kTriangle));
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
    instance.InstanceMask = 0xFF;
    instance.AccelerationStructure = blas_va;
    std::memcpy(bytes + kInstanceOffset, &instance, sizeof(instance));
    std::memcpy(bytes + kRaygenOffset, ids[0], kRecordBytes);
    std::memcpy(bytes + kMissOffset, ids[1], kRecordBytes);
    std::memcpy(bytes + kHitOffset, ids[2], kRecordBytes);
    std::memcpy(bytes + kHitOffset + kRecordBytes, &kRecordValue, sizeof(kRecordValue));
    std::memcpy(bytes + kTable2 + kRaygenOffset, ids2[0], kRecordBytes);
    std::memcpy(bytes + kTable2 + kMissOffset, ids2[1], kRecordBytes);
    std::memcpy(bytes + kTable2 + kHitOffset, ids2[2], kRecordBytes);
    std::memcpy(bytes + kTable2 + kHitOffset + kRecordBytes, &kRecordValue2, sizeof(kRecordValue2));
    // Where the decoy's local root signature has closest's register: closest under it would write this word.
    std::memcpy(bytes + kTable2 + kHitOffset + kRecordBytes + sizeof(UINT32), &kTrapValue, sizeof(kTrapValue));
    for (int mp = 0; mp < 2; ++mp) {
        const UINT64 table = mp ? kTable5 : kTable4;
        std::memcpy(bytes + table + kRaygenOffset, mixed_ids[mp][0], kRecordBytes);
        std::memcpy(bytes + table + kMissOffset, mixed_ids[mp][1], kRecordBytes);
        std::memcpy(bytes + table + kHitOffset, mixed_ids[mp][2], kRecordBytes);
        std::memcpy(bytes + table + kHitOffset + kRecordBytes, &kMixedValues[mp], sizeof(UINT32));
    }
    // The third table: every record carries kTrapValue where the trap's b0 space4 constant lies, the word after the
    // identifier for raygen and miss, the second for the hit group, whose first is its own constant. No shader reads
    // b0 space4, and none may read kTrapValue.
    const UINT64 records3[3] = {kRaygenOffset, kMissOffset, kHitOffset};
    for (int i = 0; i < 3; ++i) {
        BYTE* record = bytes + kTable3 + records3[i];
        std::memcpy(record, ids3[i], kRecordBytes);
        std::memcpy(record + kRecordBytes, i == 2 ? &kRecordValue3 : &kTrapValue, sizeof(UINT32));
        std::memcpy(record + kRecordBytes + sizeof(UINT32), &kTrapValue, sizeof(UINT32));
    }
    // The sixth table, the collection-only executable's; the seventh, the child's: the parent's raygen and hit group
    // identifiers (taken before the parent was destroyed) and miss_far's in the miss record.
    const struct {
        UINT64 table;
        const BYTE* raygen;
        const BYTE* miss;
        const BYTE* hit;
        UINT32 value;
    } later_tables[2] = {
        {kTable6, importer_ids[0], importer_ids[1], importer_ids[2], kImportValue},
        {kTable7, parent_ids[0], miss_far, parent_ids[2], kGrowValue},
    };
    for (const auto& later : later_tables) {
        std::memcpy(bytes + later.table + kRaygenOffset, later.raygen, kRecordBytes);
        std::memcpy(bytes + later.table + kMissOffset, later.miss, kRecordBytes);
        std::memcpy(bytes + later.table + kHitOffset, later.hit, kRecordBytes);
        std::memcpy(bytes + later.table + kHitOffset + kRecordBytes, &later.value, sizeof(UINT32));
    }
    env.core.pfnUnmapHeap(device.h(), upload.hheap());

    // 9, the command signature of one DISPATCH_RAYS argument (no root signature: it changes no root argument) and the
    // indirect dispatches' buffers. Two records over the first pipeline's table, the first 8x4 (rows 0 to 3), the
    // second 8x8; where both write, they write the same word. The stride's padding is zero: an engine stepping 104
    // bytes would read the second record's dimensions from zero words and launch nothing.
    const D3D12DDI_INDIRECT_ARGUMENT_DESC rays_argument{D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS, {}};
    const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 rays_signature_args{kRaysStride, 1, &rays_argument,
                                                                       D3D12DDI_HROOTSIGNATURE{}, 1};
    void* rays_signature_storage =
        env.storage.alloc(env.core.pfnCalcPrivateCommandSignatureSize(device.h(), &rays_signature_args));
    const D3D12DDI_HCOMMANDSIGNATURE hrays_signature{rays_signature_storage};
    const HRESULT hr_rays_signature =
        rays_signature_storage ? env.core.pfnCreateCommandSignature(device.h(), &rays_signature_args, hrays_signature)
                               : E_OUTOFMEMORY;
    Buffer indirect_upload, indirect_args, indirect_out, indirect_back;
    const HRESULT hr_indirect[] = {
        create_buffer(env, device, HeapKind::Upload, kIndirectUploadBytes, false, indirect_upload),
        create_buffer(env, device, HeapKind::Default, kIndirectArgsBytes, false, indirect_args),
        create_buffer(env, device, HeapKind::Default, kIndirectOutBytes, true, indirect_out),
        create_buffer(env, device, HeapKind::Readback, kIndirectOutBytes, false, indirect_back),
    };
    bool indirect_ready = hr_rays_signature == S_OK && !device.shell.device_errors;
    for (HRESULT h : hr_indirect) indirect_ready = indirect_ready && h == S_OK;
    const D3D12DDI_GPU_VIRTUAL_ADDRESS indirect_out_va =
        indirect_ready ? env.core.pfnCheckResourceVirtualAddress(device.h(), indirect_out.hres()) : 0;
    void* indirect_cpu = nullptr;
    indirect_ready = indirect_ready && indirect_out_va &&
                     env.core.pfnMapHeap(device.h(), indirect_upload.hheap(), &indirect_cpu) == S_OK && indirect_cpu;
    if (indirect_ready) {
        auto* p = static_cast<BYTE*>(indirect_cpu);
        std::memset(p, 0, static_cast<size_t>(kIndirectUploadBytes));
        for (UINT r = 0; r < 2; ++r) {
            D3D12_DISPATCH_RAYS_DESC record{};
            record.RayGenerationShaderRecord = {upload_va + kRaygenOffset, kRecordBytes};
            record.MissShaderTable = {upload_va + kMissOffset, kRecordBytes, kRecordBytes};
            record.HitGroupTable = {upload_va + kHitOffset, kHitStride, kHitStride};
            record.Width = kGrid;
            record.Height = r ? kGrid : kGrid / 2;
            record.Depth = 1;
            std::memcpy(p + r * kRaysStride, &record, sizeof(record));
        }
        std::memcpy(p + kCountsOffset, kCounts, sizeof(kCounts));
        for (UINT64 at = kPrefillOffset; at < kIndirectUploadBytes; at += sizeof(UINT32))
            std::memcpy(p + at, &kPrefill, sizeof(UINT32));
        env.core.pfnUnmapHeap(device.h(), indirect_upload.hheap());
    }
    checkf(indirect_ready,
           "raytracing pipeline: CreateCommandSignature of one DISPATCH_RAYS argument, stride %u, no root signature: "
           "S_OK; two records, the counts and the prefill in an UPLOAD buffer, a DEFAULT buffer for their copy, an "
           "output and a READBACK buffer (hr %08lx)",
           kRaysStride, static_cast<unsigned long>(hr_rays_signature));

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    Recording rec;
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, rec);
    checkf(hr == S_OK && queue && rec.table == 0,
           "raytracing pipeline: COMPUTE engine queue and a COMPUTE list on the compute table (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK || rec.table != 0) return;
    const D3D12DDIARG_RESOURCE_BARRIER_0022 begin[] = {
        transition(scratch, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(out, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    t.pfnResourceBarrier(rec.hlist(), 2, begin);
    const D3D12DDIARG_RESOURCE_BARRIER_0022 uav = uav_barrier();
    D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 build{};
    build.DestAccelerationStructureData = blas_va;
    build.Inputs = bottom;
    build.ScratchAccelerationStructureData = scratch_va;
    t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build);
    t.pfnResourceBarrier(rec.hlist(), 1, &uav);
    build.DestAccelerationStructureData = tlas_va;
    build.Inputs = top;
    t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build);
    t.pfnResourceBarrier(rec.hlist(), 1, &uav);
    t.pfnSetComputeRootSignature(rec.hlist(), D3D12DDI_HROOTSIGNATURE{rs.global});
    t.pfnSetPipelineState1(rec.hlist(), hso);
    t.pfnSetComputeRootShaderResourceView(rec.hlist(), 0, tlas_va);
    t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va);
    t.pfnSetComputeRoot32BitConstant(rec.hlist(), kMissParameter, kMiss, 0);
    D3D12DDIARG_DISPATCH_RAYS_0054 rays{};
    rays.RayGenerationShaderRecord = {upload_va + kRaygenOffset, kRecordBytes};
    rays.MissShaderTable = {upload_va + kMissOffset, kRecordBytes, kRecordBytes};
    rays.HitGroupTable = {upload_va + kHitOffset, kHitStride, kHitStride};
    rays.Width = kGrid;
    rays.Height = kGrid;
    rays.Depth = 1;
    t.pfnDispatchRays(rec.hlist(), &rays);
    // 4, the dispatch: the second pipeline over its own table, into the second half of the output.
    t.pfnSetPipelineState1(rec.hlist(), hso2);
    t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va + 256);
    D3D12DDIARG_DISPATCH_RAYS_0054 rays2 = rays;
    rays2.RayGenerationShaderRecord.StartAddress += kTable2;
    rays2.MissShaderTable.StartAddress += kTable2;
    rays2.HitGroupTable.StartAddress += kTable2;
    t.pfnDispatchRays(rec.hlist(), &rays2);
    // 5, the dispatch: the third pipeline over its own table (records of both words), into the third result.
    t.pfnSetPipelineState1(rec.hlist(), hso3);
    t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va + 512);
    D3D12DDIARG_DISPATCH_RAYS_0054 rays3 = rays;
    rays3.RayGenerationShaderRecord = {upload_va + kTable3 + kRaygenOffset, kRecordBytes + 2 * sizeof(UINT32)};
    rays3.MissShaderTable = {upload_va + kTable3 + kMissOffset, kHitStride, kHitStride};
    rays3.HitGroupTable.StartAddress += kTable3;
    t.pfnDispatchRays(rec.hlist(), &rays3);
    // 6, the dispatches: each mixed-library pipeline over its own table, into the fourth and fifth results.
    for (int mp = 0; mp < 2; ++mp) {
        const UINT64 table = mp ? kTable5 : kTable4;
        t.pfnSetPipelineState1(rec.hlist(), D3D12DDI_HSTATEOBJECT_0054{mixed_storage[mp]});
        t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va + 768 + 256 * mp);
        D3D12DDIARG_DISPATCH_RAYS_0054 mixed_rays = rays;
        mixed_rays.RayGenerationShaderRecord.StartAddress += table;
        mixed_rays.MissShaderTable.StartAddress += table;
        mixed_rays.HitGroupTable.StartAddress += table;
        t.pfnDispatchRays(rec.hlist(), &mixed_rays);
    }
    // 7 and 8, the dispatches: the collection-only executable after its collection's destroy, and the child after its
    // parent's, into the sixth and seventh results.
    const D3D12DDI_HSTATEOBJECT_0054 later_objects[2] = {himporter, hchild};
    for (int k = 0; k < 2; ++k) {
        t.pfnSetPipelineState1(rec.hlist(), later_objects[k]);
        t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va + 1280 + 256 * k);
        D3D12DDIARG_DISPATCH_RAYS_0054 later_rays = rays;
        later_rays.RayGenerationShaderRecord.StartAddress += later_tables[k].table;
        later_rays.MissShaderTable.StartAddress += later_tables[k].table;
        later_rays.HitGroupTable.StartAddress += later_tables[k].table;
        t.pfnDispatchRays(rec.hlist(), &later_rays);
    }
    // 9, the indirect dispatches: the first pipeline again, each variant into its own result of the prefilled output,
    // first over the UPLOAD records with no INDIRECT_ARGUMENT transition before them in the list (an engine may patch
    // them ahead of the list), then over their DEFAULT copy behind one (patched in the list, between its commands).
    if (indirect_ready) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_copy[] = {
            transition(indirect_out, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST),
            transition(indirect_args, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST),
        };
        t.pfnResourceBarrier(rec.hlist(), 2, to_copy);
        D3D12DDIARG_BUFFER_PLACEMENT to{}, from{};
        to.BaseAddress.UMD = {indirect_out.hres(), 0};
        from.BaseAddress.UMD = {indirect_upload.hres(), kPrefillOffset};
        t.pfnCopyBufferRegion(rec.hlist(), to, from, kIndirectOutBytes);
        to.BaseAddress.UMD = {indirect_args.hres(), 0};
        from.BaseAddress.UMD = {indirect_upload.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), to, from, kIndirectArgsBytes);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_uav =
            transition(indirect_out, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_uav);
        t.pfnSetPipelineState1(rec.hlist(), hso);
        for (UINT source = 0; source < 2; ++source) {
            if (source) {
                const D3D12DDIARG_RESOURCE_BARRIER_0022 to_indirect = transition(
                    indirect_args, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT);
                t.pfnResourceBarrier(rec.hlist(), 1, &to_indirect);
            }
            const Buffer& records = source ? indirect_args : indirect_upload;
            for (UINT v = 0; v < kIndirectVariants; ++v) {
                t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1,
                                                       indirect_out_va + 256 * (source * kIndirectVariants + v));
                D3D12DDIARG_BUFFER_PLACEMENT arguments{}, count{};
                arguments.BaseAddress.UMD = {records.hres(), 0};
                if (kIndirect[v].count != kNoCount)
                    count.BaseAddress.UMD = {records.hres(), kCountsOffset + kIndirect[v].count * sizeof(UINT32)};
                t.pfnExecuteIndirect(rec.hlist(), hrays_signature, kIndirect[v].max_count, arguments, count);
            }
        }
        const D3D12DDIARG_RESOURCE_BARRIER_0022 done =
            transition(indirect_out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &done);
        to.BaseAddress.UMD = {indirect_back.hres(), 0};
        from.BaseAddress.UMD = {indirect_out.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), to, from, kIndirectOutBytes);
    }
    const D3D12DDIARG_RESOURCE_BARRIER_0022 end =
        transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
    t.pfnResourceBarrier(rec.hlist(), 1, &end);
    D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
    dst.BaseAddress.UMD = {readback.hres(), 0};
    src.BaseAddress.UMD = {out.hres(), 0};
    t.pfnCopyBufferRegion(rec.hlist(), dst, src, kPipelineOutBytes);
    t.pfnCloseCommandList(rec.hlist());
    const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
    hr = engine_ddi::execute_command_lists(queue, 1, lists);
    checkf(hr == S_OK && !device.shell.list_errors && !device.shell.device_errors,
           "raytracing pipeline: bottom and top level, SetPipelineState1 and DispatchRays 8x8 over a shader table in "
           "the UPLOAD buffer, once for each pipeline, and the indirect dispatches, executed (hr %08lx, %u list errors, "
           "%u device errors)",
           static_cast<unsigned long>(hr), device.shell.list_errors, device.shell.device_errors);
    if (hr == S_OK && wait_queue_idle(env, queue, "raytracing pipeline")) {
        void* mapped = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &mapped);
        checkf(hr == S_OK && mapped, "raytracing pipeline: MapHeap of the READBACK heap (hr %08lx)",
               static_cast<unsigned long>(hr));
        if (hr == S_OK && mapped) {
            const auto* words = static_cast<const UINT32*>(mapped);
            UINT first = 0, first2 = 0;
            const UINT bad = mismatches(words, expected, &first);
            checkf(bad == 0,
                   "raytracing pipeline: DispatchRays writes the 64 expected words, %u hits with the hit group's local "
                   "root constant %08x and %u misses with %u (%u differ, first at %u)",
                   hits, kRecordValue, kWords - hits, kMiss, bad, first);
            UINT32 expected2[kWords];
            for (UINT i = 0; i < kWords; ++i) expected2[i] = expected[i] == kRecordValue ? kRecordValue2 : expected[i];
            const UINT bad2 = mismatches(words + 256 / sizeof(UINT32), expected2, &first2);
            checkf(bad2 == 0,
                   "raytracing pipeline: the pipeline with the decoy resolved by mangled name writes its 64 expected "
                   "words, every hit with the word its own local root signature places after the identifier, %08x, not "
                   "the next one (%08x), where the decoy's has that register; the decoy is a summary export with no DXIL "
                   "function behind it (%u differ, first at %u)",
                   kRecordValue2, kTrapValue, bad2, first2);
            UINT32 expected3[kWords];
            for (UINT i = 0; i < kWords; ++i) expected3[i] = expected[i] == kRecordValue ? kRecordValue3 : expected[i];
            const UINT32* words3 = words + 512 / sizeof(UINT32);
            UINT first3 = 0, trapped_words = 0;
            const UINT bad3 = mismatches(words3, expected3, &first3);
            for (UINT i = 0; i < kWords; ++i) trapped_words += words3[i] == kTrapValue ? 1u : 0u;
            checkf(bad3 == 0,
                   "raytracing pipeline: the trap pipeline, with the empty explicit default in place, writes its 64 "
                   "expected words: %u misses the global root constant %u and %u hits the trap's first constant %08x "
                   "(%u differ, first at %u; %u words %08x)",
                   kWords - hits, kMiss, hits, kRecordValue3, bad3, first3, trapped_words, kTrapValue);
            for (int mp = 0; mp < 2; ++mp) {
                UINT32 expected_mixed[kWords];
                for (UINT i = 0; i < kWords; ++i)
                    expected_mixed[i] = expected[i] == kRecordValue ? kMixedValues[mp] : expected[i];
                UINT first_mixed = 0;
                const UINT bad_mixed = mismatches(words + (768 + 256 * mp) / sizeof(UINT32), expected_mixed, &first_mixed);
                checkf(bad_mixed == 0,
                       "raytracing pipeline: the mixed-library pipeline, the listing library %s, writes its 64 expected "
                       "words, every hit with closest's local root constant %08x: the association by the listed mangled "
                       "name reached closest (%u differ, first at %u)",
                       mp ? "second" : "first", kMixedValues[mp], bad_mixed, first_mixed);
            }
            UINT32 expected_import[kWords], expected_grown[kWords];
            for (UINT i = 0; i < kWords; ++i) {
                expected_import[i] = expected[i] == kRecordValue ? kImportValue : expected[i];
                expected_grown[i] = expected[i] == kRecordValue ? kGrowValue : kMissFar;
            }
            UINT first_import = 0, first_grown = 0;
            const UINT bad_import = mismatches(words + 1280 / sizeof(UINT32), expected_import, &first_import);
            const UINT bad_grown = mismatches(words + 1536 / sizeof(UINT32), expected_grown, &first_grown);
            checkf(bad_import == 0,
                   "raytracing pipeline: the collection-only executable, used after its collection's DestroyStateObject, "
                   "writes its 64 expected words, %u hits with %08x and %u misses with %u (%u differ, first at %u)",
                   hits, kImportValue, kWords - hits, kMiss, bad_import, first_import);
            checkf(bad_grown == 0,
                   "raytracing pipeline: the grown pipeline, used after its parent's DestroyStateObject, over a table "
                   "of the parent's raygen and hit group identifiers and the new miss_far's writes its 64 expected "
                   "words, %u hits with %08x and %u misses with miss_far's %u (%u differ, first at %u)",
                   hits, kGrowValue, kWords - hits, kMissFar, bad_grown, first_grown);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
        void* indirect_mapped = nullptr;
        const HRESULT hr_indirect_map =
            indirect_ready ? env.core.pfnMapHeap(device.h(), indirect_back.hheap(), &indirect_mapped) : E_ABORT;
        checkf(!indirect_ready || (hr_indirect_map == S_OK && indirect_mapped),
               "raytracing pipeline: MapHeap of the indirect dispatches' READBACK heap (hr %08lx)",
               static_cast<unsigned long>(hr_indirect_map));
        if (indirect_ready && hr_indirect_map == S_OK && indirect_mapped) {
            const auto* words = static_cast<const UINT32*>(indirect_mapped);
            UINT bad[kIndirectSections], bad_first_only[kIndirectSections], first[kIndirectSections];
            bool every_record = true, first_only = true;
            for (UINT s = 0; s < kIndirectSections; ++s) {
                const IndirectVariant& v = kIndirect[s % kIndirectVariants];
                UINT32 want[kWords], want_first_only[kWords];
                rows_written(expected, v.rows, want);
                rows_written(expected, v.rows_first_only, want_first_only);
                UINT unused = 0;
                bad[s] = mismatches(words + s * kWords, want, &first[s]);
                bad_first_only[s] = mismatches(words + s * kWords, want_first_only, &unused);
                every_record = every_record && !bad[s];
                first_only = first_only && !bad_first_only[s];
            }
            // One record without a count buffer: what any engine with indirect ray dispatch does.
            checkf(!bad[0] && !bad[kIndirectVariants],
                   "raytracing pipeline: indirect DispatchRays, one 8x4 record, no count buffer: rows 0 to 3 as "
                   "DispatchRays writes them, rows 4 to 7 at the prefill, from the UPLOAD records and from their DEFAULT "
                   "copy (%u and %u words differ)",
                   bad[0], bad[kIndirectVariants]);
            if (every_record) {
                checkf(true,
                       "raytracing pipeline: indirect DispatchRays, stride %u, every record up to the smaller of count "
                       "and maximum: max 2 with count 1 rows 0 to 3, with count 2, count 3 and no count buffer all 64 "
                       "words, with count 0 none, from the UPLOAD records (patched ahead of the list) and from their "
                       "DEFAULT copy after an INDIRECT_ARGUMENT transition",
                       kRaysStride);
            } else if (first_only) {
                std::printf("SKIP  raytracing pipeline: indirect DispatchRays: the engine traces the first record alone "
                            "and nothing with a count buffer (vkd3d-proton upstream; the amdgpu-wddm engine fork "
                            "traces every record up to the count)\n");
            } else {
                // Per section, UPLOAD variants then DEFAULT ones in kIndirect's order: words differing from the oracle
                // of every record up to the count, and the first of them (64: none).
                char detail[192]{};
                int at = 0;
                for (UINT s = 0; s < kIndirectSections && at >= 0 && at < static_cast<int>(sizeof(detail)); ++s)
                    at += std::snprintf(detail + at, sizeof(detail) - static_cast<size_t>(at), "%s%u@%u", s ? " " : "",
                                        bad[s], first[s]);
                checkf(false,
                       "raytracing pipeline: indirect DispatchRays, stride %u: the results match neither every record "
                       "up to the count nor the first record alone; differing words@first per section (max 1, count "
                       "1, count 2, no count, count 0, count 3; UPLOAD then DEFAULT): %s",
                       kRaysStride, detail);
            }
            env.core.pfnUnmapHeap(device.h(), indirect_back.hheap());
        }
    }

    // Refusals. A create answers with its result and reports nothing; its record is inert and destroyable.
    const auto refused_create = [&](const char* what, HRESULT expected_hr, const D3D12DDI_STATE_SUBOBJECT_0054& extra) {
        PipelineDesc bad;
        describe(bad, rs.global, rs.local, library.code.data(), true);
        bad.subobjects[kDescribed] = extra;
        bad.args.NumSubobjects = kDescribed + 1;
        const uint32_t before = device.shell.device_errors;
        void* storage = nullptr;
        int rt = 0;
        const HRESULT hr_bad = create_state_object(env, device, bad.args, &storage, &rt);
        // The inert record still names its device's shell (engine-ddi.h, state_object_shell), until destroyed.
        const D3D12DDI_HSTATEOBJECT_0054 h{storage};
        const bool owner = storage && engine_ddi::state_object_shell(h) == &device.shell;
        if (storage) env.core.pfnDestroyStateObject(device.h(), h);
        const bool gone = storage && !engine_ddi::state_object_shell(h);
        checkf(hr_bad == expected_hr && device.shell.device_errors == before && owner && gone,
               "raytracing pipeline: %s: CreateStateObject answers %08lx (hr %08lx, %u device errors), the inert "
               "record's state_object_shell names the device's shell (%s), then none once destroyed (%s)",
               what, static_cast<unsigned long>(expected_hr), static_cast<unsigned long>(hr_bad),
               device.shell.device_errors - before, owner ? "yes" : "no", gone ? "yes" : "no");
    };
    const D3D12DDI_NODE_MASK_0054 mask{1};
    refused_create("a subobject of an unknown type (4, unused in the DDI)", E_INVALIDARG,
                   {static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(4), &mask});
    const D3D12DDI_EXISTING_COLLECTION_DESC_0054 not_collection{hso, 0, nullptr};
    refused_create("a ray tracing pipeline named as an existing collection", E_INVALIDARG,
                   {D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &not_collection});
    // The decoy with no mangled name (its only name is the plain one closest also carries), and the decoy with both of
    // closest's names: named by either, the decoy's association would reach closest too. The create is refused
    // instead, before the engine sees it.
    const struct {
        LPCWSTR mangled;
        const char* what;
    } shared_names[] = {
        {nullptr, "the second without a mangled name"},
        {kClosestMangled, "both with the same mangled name"},
    };
    for (const auto& shape : shared_names) {
        PipelineDesc shared;
        describe(shared, rs.global, rs.local, container.data(), false);
        add_decoy(shared, rs.decoy, shape.mangled);
        const uint32_t before = device.shell.device_errors;
        void* storage = nullptr;
        int rt = 0;
        const HRESULT hr_shared = create_state_object(env, device, shared.args, &storage, &rt);
        if (storage) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{storage});
        checkf(hr_shared == E_INVALIDARG && device.shell.device_errors == before,
               "raytracing pipeline: two summary exports sharing the plain name \"closest\" with different local root "
               "signatures, %s: CreateStateObject answers 80070057, never a broadened association (hr %08lx, %u "
               "device errors)",
               shape.what, static_cast<unsigned long>(hr_shared), device.shell.device_errors - before);
    }

    // One error on the calling list, then the count is put back.
    const auto refused = [&](const char* what, auto call) {
        const uint32_t before = device.shell.list_errors;
        device.shell.last_list_error = S_OK;
        device.shell.last_list = nullptr;
        call();
        checkf(device.shell.list_errors == before + 1 && device.shell.last_list_error == E_INVALIDARG &&
                   device.shell.last_list == rec.rtlist().handle,
               "raytracing pipeline: %s: one E_INVALIDARG on the calling list (%u errors, last %08lx)", what,
               device.shell.list_errors - before, static_cast<unsigned long>(device.shell.last_list_error));
        device.shell.list_errors = before;
    };
    refused("DispatchRays on a closed list", [&] { t.pfnDispatchRays(rec.hlist(), &rays); });
    {
        // A state object of a second device context over the same engine device: its own root signatures, its own
        // records. The list of the first device context refuses it.
        Device other;
        HRESULT hr_other = open_device(env, other);
        RootSignatures other_rs;
        void* other_storage = nullptr;
        int other_rt = 0;
        if (hr_other == S_OK) other_rs = create_pipeline_root_signatures(env, other);
        if (hr_other == S_OK) hr_other = other_rs.hr;
        PipelineDesc other_desc;
        describe(other_desc, other_rs.global, other_rs.local, library.code.data(), true);
        if (hr_other == S_OK) hr_other = create_state_object(env, other, other_desc.args, &other_storage, &other_rt);
        checkf(hr_other == S_OK && !other.shell.device_errors,
               "raytracing pipeline: the same state object on a second device context (hr %08lx)",
               static_cast<unsigned long>(hr_other));
        const D3D12DDIARG_RESETCOMMANDLIST_0040 reset{D3D12DDI_HCOMMANDRECORDER_0040{rec.recorder}, 1,
                                                     D3D12DDI_COMMAND_LIST_FLAG_NONE};
        t.pfnResetCommandList(rec.hlist(), &reset);
        refused("SetPipelineState1 with a state object of another device context",
                [&] { t.pfnSetPipelineState1(rec.hlist(), D3D12DDI_HSTATEOBJECT_0054{other_storage}); });
        t.pfnCloseCommandList(rec.hlist());
        if (other_storage) env.core.pfnDestroyStateObject(other.h(), D3D12DDI_HSTATEOBJECT_0054{other_storage});
        destroy_root_signatures(env, other, other_rs);
        if (other.context) {
            uint32_t live = UINT32_MAX;
            const HRESULT hr_close = engine_ddi::destroy_device_context(other.context, &live);
            checkf(hr_close == S_OK && live == 0 && !other.shell.device_errors,
                   "raytracing pipeline: the second device context closes with no live object (hr %08lx, %u live)",
                   static_cast<unsigned long>(hr_close), live);
        }
    }

    destroy_recording(env, device, rec);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "raytracing pipeline: destroy_engine_queue reports Retired");
    const bool live_owner = engine_ddi::state_object_shell(hso) == &device.shell;
    env.core.pfnDestroyStateObject(device.h(), hso);
    checkf(live_owner && !engine_ddi::state_object_shell(hso),
           "raytracing pipeline: state_object_shell names the device's shell for a live state object, none after "
           "DestroyStateObject");
    env.core.pfnDestroyStateObject(device.h(), hso2);
    env.core.pfnDestroyStateObject(device.h(), hso3);
    for (void* storage : mixed_storage) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{storage});
    env.core.pfnDestroyStateObject(device.h(), himporter);
    env.core.pfnDestroyStateObject(device.h(), hchild);
    if (hr_rays_signature == S_OK) env.core.pfnDestroyCommandSignature(device.h(), hrays_signature);
    for (Buffer* b : {&indirect_upload, &indirect_args, &indirect_out, &indirect_back}) destroy_buffer(env, device, *b);
    for (Buffer* b : {&blas, &tlas, &scratch, &out, &readback, &upload}) destroy_buffer(env, device, *b);
    destroy_root_signatures(env, device, rs);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && m.frees == m.allocations && !device.shell.device_errors &&
               !device.shell.list_errors,
           "raytracing pipeline: destroy_device_context S_OK with no live object, %u of %u allocations freed, no error "
           "but the refusals' (hr %08lx, %u live, %u device, %u list errors)",
           m.frees, m.allocations, static_cast<unsigned long>(hr), live, device.shell.device_errors,
           device.shell.list_errors);
}

} // namespace harness
