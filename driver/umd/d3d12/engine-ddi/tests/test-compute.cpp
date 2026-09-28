// SPDX-License-Identifier: MIT
// Round trip 2: a compute dispatch through the DDI. The root signature goes in as the DDI's parsed description and
// is serialized by engine-ddi; its RTS0 part must equal, byte for byte, the one dxc embedded in the fixture shader
// from the same root signature string. The shader goes in through CreateComputeShader in the DDI form, its DXIL part
// alone (test-shaders.cpp), and engine-ddi rebuilds the container the engine compiles. A UAV descriptor at slot 3 of
// a shader-visible heap, a dispatch on a COMPUTE engine queue, a copy to a READBACK buffer and a word-exact compare
// with what the shader writes: output[i] = i * 2654435761 + seed.
#include "harness.h"
#include "fixture-cs.h"
#include <cstdio>
#include <cstring>

namespace harness {

namespace {
constexpr UINT kWords = 16384;
constexpr UINT kSeed = 0x5eed;
constexpr UINT kSlot = 3;

// The RTS0 part of a DXBC container, or an empty range.
struct Part {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};
Part find_part(const uint8_t* container, size_t bytes, uint32_t fourcc) {
    auto read = [&](size_t offset) {
        uint32_t v = 0;
        if (offset + 4 <= bytes) std::memcpy(&v, container + offset, 4);
        return v;
    };
    if (bytes < 32 || read(0) != 0x43425844u) return {};
    const uint32_t count = read(28);
    for (uint32_t i = 0; i < count && 32 + size_t{4} * i + 4 <= bytes; ++i) {
        const uint32_t offset = read(32 + size_t{4} * i);
        if (offset + size_t{8} > bytes || read(offset) != fourcc) continue;
        const uint32_t size = read(offset + 4);
        if (offset + size_t{8} + size > bytes) return {};
        return {container + offset + 8, size};
    }
    return {};
}

void dump(const char* what, const Part& p) {
    std::printf("     %s RTS0, %u bytes:", what, p.size);
    for (uint32_t i = 0; i + 4 <= p.size; i += 4) {
        uint32_t v;
        std::memcpy(&v, p.data + i, 4);
        std::printf(" %08x", v);
    }
    std::printf("\n");
}
} // namespace

void test_compute(Env& env, Device& device) {
    // Root signature "DescriptorTable(UAV(u0)), RootConstants(num32BitConstants = 1, b0)", version 1.1. dxc
    // serializes the unspecified range flags as NONE (the 1.1 defaults apply when the engine reads them).
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
    const D3D12DDI_ROOT_SIGNATURE_0013 rs{2, params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};

    std::vector<uint8_t> blob;
    const HRESULT hr_ser = engine_ddi::serialize_root_signature(&rs, blob);
    const Part ours = find_part(blob.data(), blob.size(), 0x30535452u);            // "RTS0"
    const Part fixture = find_part(g_engine_test_cs, sizeof(g_engine_test_cs), 0x30535452u);
    const bool same = hr_ser == S_OK && ours.size && ours.size == fixture.size &&
                      !std::memcmp(ours.data, fixture.data, ours.size);
    checkf(same, "compute: the serialized root signature equals the RTS0 part dxc embedded (%u and %u bytes)", ours.size,
           fixture.size);
    if (!same) {
        dump("engine-ddi", ours);
        dump("fixture", fixture);
    }

    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 rs_args{};
    rs_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
    rs_args.pRootSignature_1_1 = &rs;
    void* rs_storage = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &rs_args));
    const D3D12DDI_HROOTSIGNATURE hrs{rs_storage};
    HRESULT hr = rs_storage ? env.core.pfnCreateRootSignature(device.h(), &rs_args, hrs) : E_OUTOFMEMORY;
    checkf(hr == S_OK, "compute: CreateRootSignature from the DDI description (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    // The shader in the DDI form: dxc's DXIL part alone, no signature entries. engine-ddi rebuilds the container.
    DdiShader cs;
    const bool stripped = ddi_form(g_engine_test_cs, sizeof(g_engine_test_cs), cs);
    checkf(stripped && cs.input.empty() && cs.output.empty(),
           "compute: dxc cs_6_0 container reduced to its DXIL part (%zu DWORDs) and no signature entries", cs.code.size());
    const uint32_t errors_before = device.shell.device_errors;
    void* cs_storage = stripped ? create_shader(env, device, env.core.pfnCreateComputeShader, cs, hrs) : nullptr;
    const D3D12DDI_HSHADER hcs{cs_storage};
    checkf(cs_storage && device.shell.device_errors == errors_before,
           "compute: CreateComputeShader takes the DXIL program through the native intake");

    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 pso_args{};
    pso_args.hComputeShader = hcs;
    pso_args.hRootSignature = hrs;
    void* pso_storage = env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &pso_args));
    int pso_rt = 0;
    const D3D12DDI_HPIPELINESTATE hpso{pso_storage};
    hr = pso_storage ? env.core.pfnCreatePipelineState(device.h(), &pso_args, hpso, D3D12DDI_HRTPIPELINESTATE{&pso_rt})
                     : E_OUTOFMEMORY;
    checkf(hr == S_OK, "compute: CreatePipelineState (hr %08lx)", static_cast<unsigned long>(hr));

    constexpr UINT64 kBytes = UINT64{kWords} * 4;
    Buffer out, readback;
    const HRESULT hr_o = create_buffer(env, device, HeapKind::Default, kBytes, true, out);
    const HRESULT hr_r = create_buffer(env, device, HeapKind::Readback, kBytes, false, readback);
    checkf(hr_o == S_OK && hr_r == S_OK, "compute: DEFAULT UAV buffer and READBACK buffer (hr %08lx %08lx)",
           static_cast<unsigned long>(hr_o), static_cast<unsigned long>(hr_r));

    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                                      D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    void* heap_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &heap_args));
    const D3D12DDI_HDESCRIPTORHEAP hheap{heap_storage};
    const HRESULT hr_h = heap_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &heap_args, hheap) : E_OUTOFMEMORY;
    const UINT increment = env.core.pfnGetDescriptorSizeInBytes(device.h(), D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu{};
    if (hr_h == S_OK) {
        cpu = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hheap);
        gpu = env.core.pfnGetGPUDescriptorHandleForHeapStart(device.h(), hheap);
    }
    checkf(hr_h == S_OK && increment && cpu.ptr && gpu.ptr,
           "compute: shader-visible CBV_SRV_UAV heap of 4, increment %u, CPU and GPU starts", increment);
    if (hr != S_OK || hr_o != S_OK || hr_r != S_OK || hr_h != S_OK || !cpu.ptr || !gpu.ptr) return;
    cpu.ptr += SIZE_T{kSlot} * increment;
    gpu.ptr += UINT64{kSlot} * increment;

    D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav{};
    uav.hDrvResource = out.hres();
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ResourceDimension = D3D12DDI_RD_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = kWords;
    uav.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
    env.core.pfnCreateUnorderedAccessView(device.h(), &uav, cpu);
    checkf(!device.shell.device_errors, "compute: raw R32_TYPELESS buffer UAV at slot %u", kSlot);

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "compute: create_engine_queue COMPUTE (hr %08lx)", static_cast<unsigned long>(hr));

    Recording rec;
    if (hr == S_OK) {
        hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, rec);
        checkf(hr == S_OK && rec.table == 0, "compute: COMPUTE list bound to the compute table (hr %08lx, table %u)",
               static_cast<unsigned long>(hr), rec.table);
    }
    if (hr == S_OK && rec.table == 0) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[0];
        D3D12DDI_HDESCRIPTORHEAP heaps[] = {hheap};
        t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
        t.pfnSetComputeRootSignature(rec.hlist(), hrs);
        t.pfnSetPipelineState(rec.hlist(), hpso);
        t.pfnSetComputeRootDescriptorTable(rec.hlist(), 0, gpu);
        t.pfnSetComputeRoot32BitConstant(rec.hlist(), 1, kSeed, 0);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_uav =
            transition(out, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_uav);
        t.pfnDispatch(rec.hlist(), kWords / 64, 1, 1);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {out.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), dst, src, kBytes);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "compute: descriptor heap, root signature, PSO, table, constant, Dispatch(%u), copy, execute (hr %08lx)",
               kWords / 64, static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "compute");

        void* cpu_map = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu_map);
        if (hr == S_OK && cpu_map) {
            const auto* words = static_cast<const UINT32*>(cpu_map);
            UINT bad = 0, first = kWords;
            for (UINT i = 0; i < kWords; ++i) {
                if (words[i] != i * 2654435761u + kSeed) {
                    if (!bad) first = i;
                    ++bad;
                }
            }
            checkf(bad == 0, "compute: every word is i * 2654435761 + 0x5eed (%u of %u differ, first at %u)", bad, kWords,
                   first);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            checkf(false, "compute: MapHeap of the READBACK heap (hr %08lx)", static_cast<unsigned long>(hr));
        }
    }
    destroy_recording(env, device, rec);
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "compute: destroy_engine_queue reports Retired");
    env.core.pfnDestroyDescriptorHeap(device.h(), hheap);
    destroy_buffer(env, device, out);
    destroy_buffer(env, device, readback);
    env.core.pfnDestroyPipelineState(device.h(), hpso);
    env.core.pfnDestroyShader(device.h(), hcs);
    env.core.pfnDestroyRootSignature(device.h(), hrs);
    checkf(!engine_ddi::harness_live_objects(device.context) && !engine_ddi::harness_pending_releases(device.context),
           "compute: no live object and no pending release left (%u live)",
           engine_ddi::harness_live_objects(device.context));
}

} // namespace harness
