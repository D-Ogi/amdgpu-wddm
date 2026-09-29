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

    // Two small typed buffers for the view clears, one per value type, each read back on its own. The clear takes
    // the view twice, as the API requires: a CPU handle in a heap that is not shader visible and the GPU handle of
    // an equal descriptor in the bound heap.
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 plain_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                       D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* plain_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &plain_args));
    const D3D12DDI_HDESCRIPTORHEAP hplain{plain_storage};
    const HRESULT hr_p = plain_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &plain_args, hplain) : E_OUTOFMEMORY;
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE plain =
        hr_p == S_OK ? env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hplain) : D3D12DDI_CPU_DESCRIPTOR_HANDLE{};
    checkf(hr_p == S_OK && plain.ptr, "compute: CBV_SRV_UAV heap of 2 that is not shader visible (hr %08lx)",
           static_cast<unsigned long>(hr_p));
    constexpr UINT kClearWords = 1024;
    constexpr UINT64 kClearBytes = UINT64{kClearWords} * 4;
    constexpr UINT kClearUint = 0xC1EA4BADu;
    struct Cleared {
        DXGI_FORMAT format;
        UINT slot;
        UINT32 expected;
        Buffer buffer, back;
        D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu;
        bool ready;
    } cleared[2] = {{DXGI_FORMAT_R32_UINT, 0, kClearUint, {}, {}, {}, {}, false},
                    {DXGI_FORMAT_R32_FLOAT, 1, 0x3f800000u, {}, {}, {}, {}, false}};
    for (Cleared& c : cleared) {
        const HRESULT hr_b = create_buffer(env, device, HeapKind::Default, kClearBytes, true, c.buffer);
        const HRESULT hr_k = create_buffer(env, device, HeapKind::Readback, kClearBytes, false, c.back);
        c.cpu = {plain.ptr + SIZE_T{c.slot} * increment};
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE visible{cpu.ptr - SIZE_T{kSlot} * increment + SIZE_T{c.slot} * increment};
        c.gpu = {gpu.ptr - UINT64{kSlot} * increment + UINT64{c.slot} * increment};
        if (hr_b == S_OK && hr_k == S_OK && plain.ptr) {
            D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 view{};
            view.hDrvResource = c.buffer.hres();
            view.Format = c.format;
            view.ResourceDimension = D3D12DDI_RD_BUFFER;
            view.Buffer.NumElements = kClearWords;
            env.core.pfnCreateUnorderedAccessView(device.h(), &view, c.cpu);
            env.core.pfnCopyDescriptorsSimple(device.h(), 1, visible, c.cpu, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            c.ready = !device.shell.device_errors;
        }
        checkf(c.ready, "compute: typed buffer and view for the clear at slot %u (hr %08lx %08lx)", c.slot,
               static_cast<unsigned long>(hr_b), static_cast<unsigned long>(hr_k));
    }

    // The dispatch once more through a command signature, over the first half of the buffer and with another seed.
    constexpr UINT kSeedIndirect = 0x1d1ec7u;
    const D3D12DDI_INDIRECT_ARGUMENT_DESC dispatch_argument{D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH, {}};
    const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 signature_args{3 * sizeof(UINT), 1, &dispatch_argument, hrs, 1};
    void* signature_storage = env.storage.alloc(env.core.pfnCalcPrivateCommandSignatureSize(device.h(), &signature_args));
    const D3D12DDI_HCOMMANDSIGNATURE hsignature{signature_storage};
    const HRESULT hr_s =
        signature_storage ? env.core.pfnCreateCommandSignature(device.h(), &signature_args, hsignature) : E_OUTOFMEMORY;
    Buffer arguments, second;
    const HRESULT hr_a = create_buffer(env, device, HeapKind::Upload, 256, false, arguments);
    const HRESULT hr_2 = create_buffer(env, device, HeapKind::Readback, kBytes, false, second);
    bool indirect = hr_s == S_OK && hr_a == S_OK && hr_2 == S_OK;
    if (indirect) {
        void* written = nullptr;
        indirect = env.core.pfnMapHeap(device.h(), arguments.hheap(), &written) == S_OK && written;
        if (indirect) {
            const UINT groups[3] = {kWords / 128, 1, 1};
            std::memcpy(written, groups, sizeof(groups));
            env.core.pfnUnmapHeap(device.h(), arguments.hheap());
        }
    }
    checkf(indirect, "compute: command signature of one dispatch, its argument buffer and a second READBACK buffer "
                     "(hr %08lx %08lx %08lx)",
           static_cast<unsigned long>(hr_s), static_cast<unsigned long>(hr_a), static_cast<unsigned long>(hr_2));
    // A signature that changes state: a root constant, then the dispatch. The engine needs device generated
    // commands for it and refuses the signature without them (E_NOTIMPL); created, it has to execute: the
    // first quarter of the buffer with the seed from the argument buffer.
    constexpr UINT kSeedRooted = 0x0c0ffeeu;
    constexpr UINT kRootedOffset = 64;
    D3D12DDI_INDIRECT_ARGUMENT_DESC rooted_arguments[2]{};
    rooted_arguments[0].Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    rooted_arguments[0].Constant = {1, 0, 1};
    rooted_arguments[1].Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 rooted_args{4 * sizeof(UINT), 2, rooted_arguments, hrs, 1};
    void* rooted_storage = env.storage.alloc(env.core.pfnCalcPrivateCommandSignatureSize(device.h(), &rooted_args));
    const D3D12DDI_HCOMMANDSIGNATURE hrooted{rooted_storage};
    const HRESULT hr_rooted =
        rooted_storage ? env.core.pfnCreateCommandSignature(device.h(), &rooted_args, hrooted) : E_OUTOFMEMORY;
    Buffer third;
    const HRESULT hr_3 = hr_rooted == S_OK ? create_buffer(env, device, HeapKind::Readback, kBytes, false, third) : E_ABORT;
    bool rooted = indirect && hr_rooted == S_OK && hr_3 == S_OK;
    if (rooted) {
        void* written = nullptr;
        rooted = env.core.pfnMapHeap(device.h(), arguments.hheap(), &written) == S_OK && written;
        if (rooted) {
            const UINT values[4] = {kSeedRooted, kWords / 256, 1, 1};
            std::memcpy(static_cast<BYTE*>(written) + kRootedOffset, values, sizeof(values));
            env.core.pfnUnmapHeap(device.h(), arguments.hheap());
        }
    }
    checkf(hr_rooted == E_NOTIMPL || (hr_rooted == S_OK && (rooted || !indirect)),
           "compute: command signature of a root constant and a dispatch: %s (hr %08lx %08lx)",
           hr_rooted == E_NOTIMPL ? "refused, the device has no device generated commands" : "created",
           static_cast<unsigned long>(hr_rooted), static_cast<unsigned long>(hr_3));
    {
        // Refusals: a type of a later slice, and a root argument without a root signature.
        D3D12DDI_INDIRECT_ARGUMENT_DESC refused{D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS, {}};
        D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 refused_args{64, 1, &refused, hrs, 1};
        void* refused_storage = env.storage.alloc(sizeof(void*) * 8);
        const HRESULT hr_rays =
            env.core.pfnCreateCommandSignature(device.h(), &refused_args, D3D12DDI_HCOMMANDSIGNATURE{refused_storage});
        refused.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        refused.Constant = {1, 0, 1};
        refused_args.hRootSignature = D3D12DDI_HROOTSIGNATURE{};
        const HRESULT hr_rootless =
            env.core.pfnCreateCommandSignature(device.h(), &refused_args, D3D12DDI_HCOMMANDSIGNATURE{refused_storage});
        checkf(hr_rays == E_NOTIMPL && hr_rootless == E_INVALIDARG,
               "compute: a ray dispatch argument is E_NOTIMPL, a constant without a root signature E_INVALIDARG "
               "(hr %08lx %08lx)",
               static_cast<unsigned long>(hr_rays), static_cast<unsigned long>(hr_rootless));
    }

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
        for (Cleared& c : cleared) {
            if (!c.ready) continue;
            const D3D12DDIARG_RESOURCE_BARRIER_0022 clear_begin =
                transition(c.buffer, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            t.pfnResourceBarrier(rec.hlist(), 1, &clear_begin);
            if (c.format == DXGI_FORMAT_R32_UINT) {
                const UINT values[4] = {kClearUint, 0, 0, 0};
                t.pfnClearUnorderedAccessViewUint(rec.hlist(), c.gpu, c.cpu, c.buffer.hres(), values, 0, nullptr);
            } else {
                const FLOAT values[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                t.pfnClearUnorderedAccessViewFloat(rec.hlist(), c.gpu, c.cpu, c.buffer.hres(), values, 0, nullptr);
            }
            const D3D12DDIARG_RESOURCE_BARRIER_0022 clear_end =
                transition(c.buffer, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            t.pfnResourceBarrier(rec.hlist(), 1, &clear_end);
            D3D12DDIARG_BUFFER_PLACEMENT clear_dst{}, clear_src{};
            clear_dst.BaseAddress.UMD = {c.back.hres(), 0};
            clear_src.BaseAddress.UMD = {c.buffer.hres(), 0};
            t.pfnCopyBufferRegion(rec.hlist(), clear_dst, clear_src, kClearBytes);
        }
        if (indirect) {
            const D3D12DDIARG_RESOURCE_BARRIER_0022 again =
                transition(out, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            t.pfnResourceBarrier(rec.hlist(), 1, &again);
            t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
            t.pfnSetComputeRootSignature(rec.hlist(), hrs);
            t.pfnSetPipelineState(rec.hlist(), hpso);
            t.pfnSetComputeRootDescriptorTable(rec.hlist(), 0, gpu);
            t.pfnSetComputeRoot32BitConstant(rec.hlist(), 1, kSeedIndirect, 0);
            D3D12DDIARG_BUFFER_PLACEMENT from{}, no_count{};
            from.BaseAddress.UMD = {arguments.hres(), 0};
            t.pfnExecuteIndirect(rec.hlist(), hsignature, 1, from, no_count);
            const D3D12DDIARG_RESOURCE_BARRIER_0022 done =
                transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            t.pfnResourceBarrier(rec.hlist(), 1, &done);
            D3D12DDIARG_BUFFER_PLACEMENT second_dst{}, second_src{};
            second_dst.BaseAddress.UMD = {second.hres(), 0};
            second_src.BaseAddress.UMD = {out.hres(), 0};
            t.pfnCopyBufferRegion(rec.hlist(), second_dst, second_src, kBytes);
        }
        if (rooted) {
            const D3D12DDIARG_RESOURCE_BARRIER_0022 again =
                transition(out, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            t.pfnResourceBarrier(rec.hlist(), 1, &again);
            t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
            t.pfnSetComputeRootSignature(rec.hlist(), hrs);
            t.pfnSetPipelineState(rec.hlist(), hpso);
            t.pfnSetComputeRootDescriptorTable(rec.hlist(), 0, gpu);
            // Not the seed: the one the dispatch uses comes from the argument buffer.
            t.pfnSetComputeRoot32BitConstant(rec.hlist(), 1, ~kSeedRooted, 0);
            D3D12DDIARG_BUFFER_PLACEMENT from{}, no_count{};
            from.BaseAddress.UMD = {arguments.hres(), kRootedOffset};
            t.pfnExecuteIndirect(rec.hlist(), hrooted, 1, from, no_count);
            const D3D12DDIARG_RESOURCE_BARRIER_0022 done =
                transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            t.pfnResourceBarrier(rec.hlist(), 1, &done);
            D3D12DDIARG_BUFFER_PLACEMENT third_dst{}, third_src{};
            third_dst.BaseAddress.UMD = {third.hres(), 0};
            third_src.BaseAddress.UMD = {out.hres(), 0};
            t.pfnCopyBufferRegion(rec.hlist(), third_dst, third_src, kBytes);
        }
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
    if (indirect && hr == S_OK) {
        void* second_map = nullptr;
        const HRESULT hr_m = env.core.pfnMapHeap(device.h(), second.hheap(), &second_map);
        UINT bad = 0, first = kWords;
        if (hr_m == S_OK && second_map) {
            const auto* words = static_cast<const UINT32*>(second_map);
            for (UINT i = 0; i < kWords; ++i) {
                if (words[i] != i * 2654435761u + (i < kWords / 2 ? kSeedIndirect : kSeed)) {
                    if (!bad) first = i;
                    ++bad;
                }
            }
            env.core.pfnUnmapHeap(device.h(), second.hheap());
        }
        checkf(hr_m == S_OK && second_map && !bad,
               "compute: ExecuteIndirect rewrote the first half with its seed and left the second (%u differ, first "
               "at %u)",
               bad, first);
    }
    if (rooted && hr == S_OK) {
        void* third_map = nullptr;
        const HRESULT hr_m = env.core.pfnMapHeap(device.h(), third.hheap(), &third_map);
        UINT bad = 0, first = kWords;
        if (hr_m == S_OK && third_map) {
            const auto* words = static_cast<const UINT32*>(third_map);
            for (UINT i = 0; i < kWords; ++i) {
                const UINT seed = i < kWords / 4 ? kSeedRooted : i < kWords / 2 ? kSeedIndirect : kSeed;
                if (words[i] != i * 2654435761u + seed) {
                    if (!bad) first = i;
                    ++bad;
                }
            }
            env.core.pfnUnmapHeap(device.h(), third.hheap());
        }
        checkf(hr_m == S_OK && third_map && !bad,
               "compute: ExecuteIndirect with a root constant rewrote the first quarter with the seed of its "
               "argument buffer and left the rest (%u differ, first at %u)",
               bad, first);
    }
    for (Cleared& c : cleared) {
        if (!c.ready || hr != S_OK) continue;
        void* view_map = nullptr;
        const HRESULT hr_m = env.core.pfnMapHeap(device.h(), c.back.hheap(), &view_map);
        UINT bad = 0;
        if (hr_m == S_OK && view_map) {
            const auto* words = static_cast<const UINT32*>(view_map);
            for (UINT i = 0; i < kClearWords; ++i) bad += words[i] != c.expected;
            env.core.pfnUnmapHeap(device.h(), c.back.hheap());
        }
        checkf(hr_m == S_OK && view_map && !bad, "compute: view clear at slot %u wrote %08x to every word (%u differ)",
               c.slot, c.expected, bad);
    }
    destroy_recording(env, device, rec);
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "compute: destroy_engine_queue reports Retired");
    for (Cleared& c : cleared) {
        destroy_buffer(env, device, c.buffer);
        destroy_buffer(env, device, c.back);
    }
    if (hr_s == S_OK) env.core.pfnDestroyCommandSignature(device.h(), hsignature);
    if (hr_rooted == S_OK) env.core.pfnDestroyCommandSignature(device.h(), hrooted);
    if (hr_3 == S_OK) destroy_buffer(env, device, third);
    destroy_buffer(env, device, arguments);
    destroy_buffer(env, device, second);
    if (hr_p == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hplain);
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
