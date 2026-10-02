// SPDX-License-Identifier: MIT
// The once-only diagnostic lines of engine-ddi (log_refusal, so that a game run's debugger log has them), on a device
// of their own: each is written for the first case of its kind and not again, and says what that case was. The lines
// are written once a process, so they are counted over the whole run (refusal_lines).
#include "harness.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

namespace {
D3D12DDIARG_BUFFER_PLACEMENT at(const Buffer& b, UINT64 offset) {
    D3D12DDIARG_BUFFER_PLACEMENT p{};
    p.BaseAddress.UMD = {b.hres(), offset};
    return p;
}

// A committed 2D texture of one mip in a DEFAULT heap, created in the COMMON layout. Its flags decide the heap's
// category, as the runtime's do.
HRESULT create_texture(Env& env, Device& device, DXGI_FORMAT format, UINT width, UINT height,
                       D3D12DDI_RESOURCE_FLAGS_0003 flags, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = width;
    res.Height = height;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = flags;
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
    heap.Flags = (flags & (D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET | D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL))
                     ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES
                     : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt}, &res,
                                             nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

// The first virtual placement: two copies from an UPLOAD buffer through virtual layouts into an R32_UINT texture, run
// on an engine queue. Only the first is logged, with its virtual size (the footprint's) and its physical size, which
// the engine never sees; the second, at another offset and with another box, adds nothing.
void virtual_placement(Env& env, Device& device) {
    Buffer upload, texture;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, 4096, false, upload);
    const HRESULT hr_t =
        create_texture(env, device, DXGI_FORMAT_R32_UINT, 8, 4, D3D12DDI_RESOURCE_FLAG_0003_NONE, texture);
    checkf(hr_u == S_OK && hr_t == S_OK, "log lines: UPLOAD buffer and an R32_UINT 8x4 texture (hr %08lx %08lx)",
           static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_t));
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    HRESULT hr = hr_u == S_OK && hr_t == S_OK ? S_OK : E_FAIL;
    if (hr == S_OK) {
        BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    }
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    if (hr == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT first{DXGI_FORMAT_R32_UINT, 7, 3, 1, 8, 4, 1, 256, 1024};
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT second{DXGI_FORMAT_R32_UINT, 4, 2, 1, 4, 2, 1, 256, 512};
        const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        const D3D12DDIARG_BUFFER_PLACEMENT tex = at(texture, 0), from = at(upload, 0), from2 = at(upload, 1024);
        const D3D12DDI_BOX box{0, 0, 0, 7, 3, 1}, box2{0, 0, 0, 4, 2, 1};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
            transition(texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 0, 0, 0, &from,
                               {D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED, &first}, &box);
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 1, 1, 0, &from2,
                               {D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED, &second}, &box2);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors && wait_queue_idle(env, queue, "log lines"),
               "log lines: two copies through virtual placements recorded and run (hr %08lx, %u list errors)",
               static_cast<unsigned long>(hr), device.shell.list_errors);
    }
    const std::vector<std::string> lines = refusal_lines("CopyTextureRegion: first virtual placement");
    const char* want = "CopyTextureRegion: first virtual placement, source: format 42, virtual 7 x 3 x 1, physical 8 x "
                       "4 x 1, pitch 256, slice pitch 1024, offset 0, box (0, 0, 0) to (7, 3, 1)";
    checkf(lines.size() == 1 && lines[0] == want, "log lines: one line for the first virtual placement (%zu): \"%s\"",
           lines.size(), lines.empty() ? "" : lines[0].c_str());
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    destroy_buffer(env, device, texture);
    destroy_buffer(env, device, upload);
}

// The first refusal of each format by CheckResourceAllocationInfo: a 64 x 64 texture of each candidate the engine has
// no size for, asked twice, reports E_INVALIDARG twice and gives one line with its description; a candidate the
// engine sizes gives none. Returns the device errors reported.
uint32_t allocation_refusals(Env& env, Device& device) {
    const DXGI_FORMAT candidates[] = {DXGI_FORMAT_YUY2, DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM,
                                      DXGI_FORMAT_R1_UNORM, DXGI_FORMAT_AI44, DXGI_FORMAT_R8G8B8A8_UNORM};
    const uint32_t errors_before = device.shell.device_errors;
    unsigned refused = 0, wrong = 0;
    std::string first_line, first_want;
    for (const DXGI_FORMAT format : candidates) {
        D3D12_RESOURCE_DESC api{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, format, {1, 0},
                                D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        const bool engine_refuses = env.engine->GetResourceAllocationInfo(0, 1, &api).SizeInBytes == UINT64_MAX;
        D3D12DDIARG_CREATERESOURCE_0088 res{};
        res.ResourceType = D3D12DDI_RT_TEXTURE2D;
        res.Width = 64;
        res.Height = 64;
        res.DepthOrArraySize = 1;
        res.MipLevels = 1;
        res.Format = format;
        res.SampleDesc = {1, 0};
        res.Layout = D3D12DDI_TL_UNDEFINED;
        res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
        const uint32_t errors = device.shell.device_errors;
        for (int i = 0; i < 2; ++i) {
            D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
            env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1,
                                                    &info);
        }
        char prefix[96];
        std::snprintf(prefix, sizeof(prefix), "CheckResourceAllocationInfo: %08lx reported for the first refusal of "
                      "format %u:", static_cast<unsigned long>(E_INVALIDARG), static_cast<unsigned>(format));
        const std::vector<std::string> lines = refusal_lines(prefix);
        const uint32_t reported = device.shell.device_errors - errors;
        const bool last_invalid = device.shell.last_device_error == E_INVALIDARG;
        wrong += engine_refuses ? (lines.size() != 1 || reported != 2 || !last_invalid) : (!lines.empty() || reported);
        if (engine_refuses && !refused++ && !lines.empty()) {
            first_line = lines[0];
            first_want = std::string(prefix) + " type 3, 64 x 64, depth or array 1, mips 1, samples 1, flags 0x0, "
                                               "layout 0, castable 0, alignment 0, optimization 0x0";
        }
    }
    checkf(refused && !wrong && first_line == first_want,
           "log lines: one CheckResourceAllocationInfo line per refused format, two E_INVALIDARG reports each (%u of "
           "%zu candidates refused, %u differ); the first is \"%s\"",
           refused, sizeof(candidates) / sizeof(candidates[0]), wrong, first_line.c_str());
    return device.shell.device_errors - errors_before;
}

// Copies between a footprint and a depth-stencil texture: a D32_FLOAT 8 x 8 depth buffer filled from an UPLOAD buffer
// at offset 260 (a multiple of 4, not of 512: the first such copy, one line) and read back into a READBACK buffer at
// offset 1024 (nothing), run on an engine queue and compared; then twice at offset 258 in a second list, each refused
// by its box after the slot saw it, so the engine never gets an offset Vulkan forbids for a depth aspect (the first
// unaligned copy: one line, the second nothing). Returns the list errors those two refusals reported.
uint32_t depth_copies(Env& env, Device& device) {
    constexpr UINT kUp = 260, kDown = 1024, kPitch = 256;
    auto depth_at = [](UINT x, UINT y) { return static_cast<float>(y * 8 + x + 1) / 128.0f; };
    Buffer upload, readback, depth;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, 4096, false, upload);
    const HRESULT hr_r = create_buffer(env, device, HeapKind::Readback, 4096, false, readback);
    const HRESULT hr_d =
        create_texture(env, device, DXGI_FORMAT_D32_FLOAT, 8, 8, D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL, depth);
    checkf(hr_u == S_OK && hr_r == S_OK && hr_d == S_OK,
           "log lines: UPLOAD and READBACK buffers and a D32_FLOAT 8x8 depth buffer (hr %08lx %08lx %08lx)",
           static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_r), static_cast<unsigned long>(hr_d));
    const uint32_t list_errors = device.shell.list_errors;
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec, refused;
    HRESULT hr = hr_u == S_OK && hr_r == S_OK && hr_d == S_OK ? S_OK : E_FAIL;
    void* cpu = nullptr;
    if (hr == S_OK) hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    if (hr == S_OK && cpu) {
        for (UINT y = 0; y < 8; ++y)
            for (UINT x = 0; x < 8; ++x) {
                const float v = depth_at(x, y);
                std::memcpy(static_cast<BYTE*>(cpu) + kUp + y * kPitch + x * 4, &v, sizeof(v));
            }
        env.core.pfnUnmapHeap(device.h(), upload.hheap());
    }
    if (hr == S_OK) {
        BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    }
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, refused);
    if (hr == S_OK) {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_D32_FLOAT, 8, 8, 1, kPitch,
                                                                        8 * kPitch};
        const D3D12DDIARG_PLACED_RESOURCE pitched{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint};
        const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        const D3D12DDIARG_BUFFER_PLACEMENT tex = at(depth, 0), up = at(upload, kUp), down = at(readback, kDown),
                                           unaligned = at(upload, kUp - 2);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
            transition(depth, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(depth, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 0, 0, 0, &up, pitched, nullptr);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
        t.pfnCopyTextureRegion(rec.hlist(), &down, pitched, 0, 0, 0, &tex, subresource, nullptr);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        const bool idle = hr == S_OK && wait_queue_idle(env, queue, "log lines");
        UINT differ = 64;
        if (idle && env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            differ = 0;
            for (UINT y = 0; y < 8; ++y)
                for (UINT x = 0; x < 8; ++x) {
                    float v = 0;
                    std::memcpy(&v, static_cast<const BYTE*>(cpu) + kDown + y * kPitch + x * 4, sizeof(v));
                    if (v != depth_at(x, y)) ++differ;
                }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
        checkf(idle && device.shell.list_errors == list_errors && differ == 0,
               "log lines: a depth buffer filled from a footprint and read back into another, every value in place "
               "(hr %08lx, %u of 64 differ)",
               static_cast<unsigned long>(hr), differ);
        const D3D12DDI_BOX inverted{8, 0, 0, 0, 8, 1};      // right below left: refused after the slot saw the call
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& u = env.lists[refused.table];
        u.pfnCopyTextureRegion(refused.hlist(), &tex, subresource, 0, 0, 0, &unaligned, pitched, &inverted);
        u.pfnCopyTextureRegion(refused.hlist(), &tex, subresource, 0, 0, 0, &unaligned, pitched, &inverted);
        u.pfnCloseCommandList(refused.hlist());
    }
    const uint32_t reported = device.shell.list_errors - list_errors;
    const std::vector<std::string> aligned = refusal_lines("CopyTextureRegion: first depth-stencil copy, ");
    const std::vector<std::string> odd =
        refusal_lines("CopyTextureRegion: first depth-stencil copy with an offset not a multiple of 4");
    const char* want_aligned = "CopyTextureRegion: first depth-stencil copy, from a footprint: resource format 40, "
                               "subresource 0, plane 0, footprint format 40, offset 260 (% 4 = 0, % 512 = 260), "
                               "pitch 256";
    const char* want_odd = "CopyTextureRegion: first depth-stencil copy with an offset not a multiple of 4, from a "
                           "footprint: resource format 40, subresource 0, plane 0, footprint format 40, "
                           "offset 258 (% 4 = 2, % 512 = 258), pitch 256";
    checkf(aligned.size() == 1 && aligned[0] == want_aligned && odd.size() == 1 && odd[0] == want_odd &&
               reported == 2 && device.shell.last_list_error == E_INVALIDARG,
           "log lines: one line for the first depth-stencil copy and one for the first at an offset not a multiple of "
           "4 (%zu, %zu; %u refusals reported): \"%s\", \"%s\"",
           aligned.size(), odd.size(), reported, aligned.empty() ? "" : aligned[0].c_str(),
           odd.empty() ? "" : odd[0].c_str());
    destroy_recording(env, device, refused);
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    destroy_buffer(env, device, depth);
    destroy_buffer(env, device, readback);
    destroy_buffer(env, device, upload);
    return reported;
}

// A reserved 64 x 64 texture of one mip: neither a heap nor a base resource (engine-ddi.h, "Reserved"), in the API's
// layout for reserved textures, starting in the legacy COPY_DEST state.
HRESULT create_reserved_texture(Env& env, Device& device, DXGI_FORMAT format, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = 64;
    res.Height = 64;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = env.core.pfnCalcPrivateHeapAndResourceSizes(
        device.h(), nullptr, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.resource) return E_OUTOFMEMORY;
    const HRESULT hr = env.core.pfnCreateHeapAndResource(device.h(), nullptr, D3D12DDI_HHEAP{},
                                                         D3D12DDI_HRTRESOURCE{&out.rt}, &res, nullptr,
                                                         D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
    if (FAILED(hr)) out.resource = nullptr;             // nothing to destroy
    return hr;
}

// The first reserved texture the engine reports no tiled support for: a reserved R32_UINT texture, TILED in the
// engine, gives no line; one of the first format the engine makes 2D textures of but reports no TILED support for
// (one plane, not depth-stencil) gives one with what the slot returned, a second one nothing. The runtime would
// refuse such a texture after CheckFormatSupport; the harness asks the slot directly, and the engine makes its
// committed fallback or refuses it.
void untiled_reserved(Env& env, Device& device) {
    const auto support = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT s{f};
        if (FAILED(env.engine->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s))))
            s = D3D12_FEATURE_DATA_FORMAT_SUPPORT{f};
        return s;
    };
    const auto planes = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_FORMAT_INFO info{f, 0};
        return SUCCEEDED(env.engine->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)))
                   ? info.PlaneCount
                   : 0;
    };
    const char* prefix = "CreateHeapAndResource: first reserved texture";
    Buffer tiled;
    const bool r32_tiled = (support(DXGI_FORMAT_R32_UINT).Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0;
    const HRESULT hr_t = create_reserved_texture(env, device, DXGI_FORMAT_R32_UINT, tiled);
    const size_t after_tiled = refusal_lines(prefix).size();
    checkf(hr_t == S_OK && r32_tiled && after_tiled == 0,
           "log lines: a reserved R32_UINT texture, TILED in the engine, gives no line (hr %08lx, TILED %d, %zu lines)",
           static_cast<unsigned long>(hr_t), r32_tiled ? 1 : 0, after_tiled);
    destroy_buffer(env, device, tiled);

    DXGI_FORMAT untiled = DXGI_FORMAT_UNKNOWN;
    for (UINT v = 1; v <= DXGI_FORMAT_A4B4G4R4_UNORM && untiled == DXGI_FORMAT_UNKNOWN; ++v) {
        const auto f = static_cast<DXGI_FORMAT>(v);
        const D3D12_FEATURE_DATA_FORMAT_SUPPORT s = support(f);
        if ((s.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && !(s.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) &&
            !(s.Support2 & D3D12_FORMAT_SUPPORT2_TILED) && planes(f) == 1)
            untiled = f;
    }
    if (untiled == DXGI_FORMAT_UNKNOWN) {
        std::printf("SKIP  log lines: the engine reports TILED for every one-plane 2D texture format it makes\n");
        return;
    }
    Buffer first, second;
    const HRESULT hr_first = create_reserved_texture(env, device, untiled, first);
    const HRESULT hr_second = create_reserved_texture(env, device, untiled, second);
    const std::vector<std::string> lines = refusal_lines(prefix);
    char want[256];
    std::snprintf(want, sizeof(want),
                  "CreateHeapAndResource: first reserved texture the engine reports no tiled support for: format %u "
                  "(asked as %u), 64 x 64, array 1, mips 1, samples 1, flags 0x0, result %08lx",
                  static_cast<unsigned>(untiled), static_cast<unsigned>(untiled), static_cast<unsigned long>(hr_first));
    checkf(lines.size() == 1 && lines[0] == want,
           "log lines: one line for the first reserved texture of format %u, which the engine reports no TILED "
           "support for (hr %08lx, then %08lx; %zu lines): \"%s\"",
           static_cast<unsigned>(untiled), static_cast<unsigned long>(hr_first), static_cast<unsigned long>(hr_second),
           lines.size(), lines.empty() ? "" : lines[0].c_str());
    destroy_buffer(env, device, second);
    destroy_buffer(env, device, first);
}
} // namespace

void test_log_lines(Env& env) {
    Device device;
    HRESULT hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "log lines: device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    virtual_placement(env, device);
    const uint32_t device_errors = allocation_refusals(env, device);
    const uint32_t list_errors = depth_copies(env, device);
    untiled_reserved(env, device);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && device.shell.device_errors == device_errors &&
               device.shell.list_errors == list_errors,
           "log lines: destroy_device_context S_OK with no live object, no error reported but the refusals' (hr "
           "%08lx, %u live, %u device, %u list errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors, device.shell.list_errors);
}

} // namespace harness
