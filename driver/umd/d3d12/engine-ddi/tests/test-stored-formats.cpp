// SPDX-License-Identifier: MIT
// The packed video formats engine-ddi stores as typeless formats (internal.h, StoredFormat): AYUV, Y410, Y416 and the
// 4:2:2 YUY2, Y210, Y216, whose element holds two pixels. Each is sized as its storage, created, viewed, cleared,
// copied and read back exactly, in pixels on the DDI side and in elements on the engine's.
#include "harness.h"
#include "format-list.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace harness {

namespace {
constexpr UINT kW = 8, kH = 4, kPitch = 256;            // pixels; a row of every footprint is kPitch bytes
constexpr UINT kTexBytes = kH * kPitch;                 // one footprint, 512-aligned
constexpr UINT kCaseUp = kTexBytes, kCaseDown = 3 * kTexBytes;

struct Case {
    DXGI_FORMAT format;
    UINT pixels;                // per element
    UINT bytes;                 // per element
    DXGI_FORMAT uint_view;      // the clear of the third texture: R32_UINT for a 4-byte element
    BYTE float_clear[8];        // (1, 0, 0, 1) through the view the video format itself names
    BYTE uint_clear[8];         // through uint_view
};
// R10G10B10A2_UNORM (1, 0, 0, 1) is 0x3FF | 3 << 30; R32_UINT clears 0x12345678, R16G16B16A16_UINT 0x1234, 0x5678,
// 0x9ABC, 0xDEF0. Little endian throughout.
const Case kCases[] = {
    {DXGI_FORMAT_AYUV, 1, 4, DXGI_FORMAT_R32_UINT, {0xFF, 0, 0, 0xFF}, {0x78, 0x56, 0x34, 0x12}},
    {DXGI_FORMAT_Y410, 1, 4, DXGI_FORMAT_R32_UINT, {0xFF, 0x03, 0, 0xC0}, {0x78, 0x56, 0x34, 0x12}},
    {DXGI_FORMAT_Y416, 1, 8, DXGI_FORMAT_R16G16B16A16_UINT, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF},
     {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE}},
    {DXGI_FORMAT_YUY2, 2, 4, DXGI_FORMAT_R32_UINT, {0xFF, 0, 0, 0xFF}, {0x78, 0x56, 0x34, 0x12}},
    {DXGI_FORMAT_Y210, 2, 8, DXGI_FORMAT_R16G16B16A16_UINT, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF},
     {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE}},
    {DXGI_FORMAT_Y216, 2, 8, DXGI_FORMAT_R16G16B16A16_UINT, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF},
     {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE}},
};
constexpr UINT kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

// The uploaded byte k of element (e, y) of case c: unique within a texture.
BYTE pattern(UINT c, UINT e, UINT y, UINT k) { return static_cast<BYTE>(y * 64 + e * 8 + k + c * 3 + 1); }

D3D12DDIARG_CREATERESOURCE_0088 description(DXGI_FORMAT format, UINT64 width, UINT16 mips, UINT samples) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = width;
    res.Height = kH;
    res.DepthOrArraySize = 1;
    res.MipLevels = mips;
    res.Format = format;
    res.SampleDesc = {samples, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    return res;
}

UINT64 ddi_size(Env& env, Device& device, const D3D12DDIARG_CREATERESOURCE_0088& res) {
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    return info.ResourceDataSize;
}

// A committed kW x kH texture of the video format with ALLOW_UNORDERED_ACCESS in a DEFAULT heap.
HRESULT create_video_texture(Env& env, Device& device, DXGI_FORMAT format, Buffer& out) {
    out = Buffer{};
    const D3D12DDIARG_CREATERESOURCE_0088 res = description(format, kW, 1, 1);
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
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

D3D12DDIARG_BUFFER_PLACEMENT at(const Buffer& b, UINT64 offset) {
    D3D12DDIARG_BUFFER_PLACEMENT p{};
    p.BaseAddress.UMD = {b.hres(), offset};
    return p;
}

// What a kW x kH footprint of case c should read back: the clear element everywhere, then the rectangle of copied
// elements, from the uploaded pattern at (src_x, src_y), at (dst_x, dst_y). All in elements.
void expect(const Case& k, UINT c, const BYTE* clear, UINT dst_x, UINT dst_y, UINT src_x, UINT src_y, UINT w, UINT h,
            BYTE* out) {
    const UINT elements = kW / k.pixels;
    for (UINT y = 0; y < kH; ++y)
        for (UINT e = 0; e < elements; ++e)
            for (UINT b = 0; b < k.bytes; ++b) {
                const bool copied = e >= dst_x && e < dst_x + w && y >= dst_y && y < dst_y + h;
                out[y * kPitch + e * k.bytes + b] =
                    copied ? pattern(c, src_x + e - dst_x, src_y + y - dst_y, b) : clear ? clear[b] : 0;
            }
}

UINT mismatches(const Case& k, const BYTE* got, const BYTE* want) {
    UINT bad = 0;
    for (UINT y = 0; y < kH; ++y)
        bad += std::memcmp(got + y * kPitch, want + y * kPitch, (kW / k.pixels) * k.bytes) ? 1u : 0u;
    return bad;
}
} // namespace

void test_stored_formats(Env& env, Device& device) {
    // Sizes: each format is sized as the engine sizes its storage, an element per pixel or per two; a 4:2:2 format
    // of an odd width or of more than one mip level, and a multisample one, gets UINT64_MAX; a stored format of one
    // pixel an element keeps its mips. Support: the view format's answer within the bits the format list allows.
    unsigned wrong_size = 0;
    std::string sizes;
    for (const Case& k : kCases) {
        const engine_ddi::StoredFormat* s = engine_ddi::stored_format(k.format);
        const D3D12_RESOURCE_DESC api{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kW / k.pixels, kH, 1, 1,
                                      s ? s->storage : DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const UINT64 engine = env.engine->GetResourceAllocationInfo(0, 1, &api).SizeInBytes;
        const UINT64 ddi = ddi_size(env, device, description(k.format, kW, 1, 1));
        const UINT64 odd = ddi_size(env, device, description(k.format, kW - 1, 1, 1));
        const UINT64 mips = ddi_size(env, device, description(k.format, kW, 2, 1));
        const UINT64 ms = ddi_size(env, device, description(k.format, kW, 1, 4));
        const bool packed = k.pixels > 1;
        const bool ok = s && engine != UINT64_MAX && ddi == engine && ms == UINT64_MAX &&
                        (odd == UINT64_MAX) == packed && (mips == UINT64_MAX) == packed && odd && mips;
        wrong_size += !ok;
        char line[96];
        std::snprintf(line, sizeof(line), " %u:%llu", static_cast<unsigned>(k.format),
                      static_cast<unsigned long long>(ddi));
        sizes += line;
    }
    checkf(!wrong_size,
           "stored formats: each sized as its storage, 4:2:2 refused at an odd width or two mips, multisample refused "
           "(%u differ; sizes%s)",
           wrong_size, sizes.c_str());
    unsigned wrong_support = 0;
    for (const Case& k : kCases) {
        UINT support = 0;
        env.core.pfnCheckFormatSupport(device.h(), k.format, &support);
        UINT allowed = 0;
        for (const engine_ddi::FormatListEntry& e : engine_ddi::kFormatList)
            if (e.format == k.format) allowed = e.allowed;
        constexpr UINT kViewBits = D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_RENDERTARGET |
                                   D3D12DDI_FORMAT_SUPPORT_BLENDABLE | D3D12DDI_FORMAT_SUPPORT_UAV_WRITES |
                                   D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
        if (support != (allowed & kViewBits)) {
            if (!wrong_support++)
                checkf(false, "stored formats: format %u: support %#x, expected %#x", static_cast<unsigned>(k.format),
                       support, allowed & kViewBits);
        }
    }
    checkf(!wrong_support, "stored formats: support of each is sample, gather and typed UAV writes, AYUV also render "
                           "target and blend (%u differ)", wrong_support);

    // The round trip of each format: A filled from an UPLOAD footprint; B cleared to (1, 0, 0, 1) through a UAV that
    // names the video format (engine-ddi gives the engine the view format), then pixels (2, 0)-(6, 2) of A copied to
    // (2, 1); C cleared through the UINT view (R32_UINT for a 4-byte element) and pixels (4, 1)-(8, 3) of the footprint
    // copied to (2, 2). A, B and C read back into footprints of the video format.
    const uint32_t errors_before = device.shell.device_errors, list_errors_before = device.shell.list_errors;
    Buffer upload, readback, tex[kCaseCount][3];
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, UINT64{kCaseUp} * kCaseCount, false, upload);
    const HRESULT hr_r =
        create_buffer(env, device, HeapKind::Readback, UINT64{kCaseDown} * kCaseCount, false, readback);
    HRESULT hr_t = S_OK;
    for (UINT c = 0; c < kCaseCount; ++c)
        for (Buffer& t : tex[c]) {
            const HRESULT hr = create_video_texture(env, device, kCases[c].format, t);
            if (hr != S_OK && hr_t == S_OK) hr_t = hr;
        }
    checkf(hr_u == S_OK && hr_r == S_OK && hr_t == S_OK,
           "stored formats: UPLOAD and READBACK buffers and three %ux%u textures of each format (hr %08lx %08lx %08lx)",
           kW, kH, static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_r),
           static_cast<unsigned long>(hr_t));
    constexpr UINT kViews = 2 * kCaseCount;
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 visible_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews,
                                                         D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 plain_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews,
                                                       D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* visible_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &visible_args));
    void* plain_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &plain_args));
    const D3D12DDI_HDESCRIPTORHEAP hvisible{visible_storage}, hplain{plain_storage};
    const HRESULT hr_v =
        visible_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &visible_args, hvisible) : E_OUTOFMEMORY;
    const HRESULT hr_p =
        plain_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &plain_args, hplain) : E_OUTOFMEMORY;
    const UINT increment = env.core.pfnGetDescriptorSizeInBytes(device.h(), D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    checkf(hr_v == S_OK && hr_p == S_OK && increment,
           "stored formats: CBV_SRV_UAV heaps of %u, shader visible and not (hr %08lx %08lx)", kViews,
           static_cast<unsigned long>(hr_v), static_cast<unsigned long>(hr_p));
    if (hr_u != S_OK || hr_r != S_OK || hr_t != S_OK || hr_v != S_OK || hr_p != S_OK || !increment) {
        if (hr_v == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hvisible);
        if (hr_p == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hplain);
        for (auto& set : tex)
            for (Buffer& t : set) destroy_buffer(env, device, t);
        destroy_buffer(env, device, readback);
        destroy_buffer(env, device, upload);
        return;
    }
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE visible_cpu =
        env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hvisible);
    const D3D12DDI_GPU_DESCRIPTOR_HANDLE visible_gpu =
        env.core.pfnGetGPUDescriptorHandleForHeapStart(device.h(), hvisible);
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE plain_cpu = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hplain);
    // Slot 2c: B's view naming the video format; 2c + 1: C's UINT view.
    for (UINT c = 0; c < kCaseCount; ++c)
        for (UINT v = 0; v < 2; ++v) {
            const UINT slot = 2 * c + v;
            D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav{};
            uav.hDrvResource = tex[c][1 + v].hres();
            uav.Format = v ? kCases[c].uint_view : kCases[c].format;
            uav.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
            const D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu{plain_cpu.ptr + SIZE_T{slot} * increment};
            env.core.pfnCreateUnorderedAccessView(device.h(), &uav, cpu);
            env.core.pfnCopyDescriptorsSimple(device.h(), 1, {visible_cpu.ptr + SIZE_T{slot} * increment}, cpu,
                                              D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

    void* cpu = nullptr;
    HRESULT hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "stored formats: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr == S_OK && cpu) {
        auto* bytes = static_cast<BYTE*>(cpu);
        std::memset(bytes, 0xEE, kCaseUp * kCaseCount);
        for (UINT c = 0; c < kCaseCount; ++c)
            for (UINT y = 0; y < kH; ++y)
                for (UINT e = 0; e < kW / kCases[c].pixels; ++e)
                    for (UINT b = 0; b < kCases[c].bytes; ++b)
                        bytes[c * kCaseUp + y * kPitch + e * kCases[c].bytes + b] = pattern(c, e, y, b);
        env.core.pfnUnmapHeap(device.h(), upload.hheap());
    }
    engine_ddi::EngineQueue* queue = nullptr;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "stored formats: create_engine_queue DIRECT (hr %08lx)",
           static_cast<unsigned long>(hr));
    Recording rec;
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    if (hr == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        D3D12DDI_HDESCRIPTORHEAP heaps[] = {hvisible};
        t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
        const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        auto barrier = [&](const Buffer& b, D3D12DDI_RESOURCE_STATES from, D3D12DDI_RESOURCE_STATES to) {
            const D3D12DDIARG_RESOURCE_BARRIER_0022 one = transition(b, from, to);
            t.pfnResourceBarrier(rec.hlist(), 1, &one);
        };
        for (UINT c = 0; c < kCaseCount; ++c) {
            const Case& k = kCases[c];
            Buffer& a = tex[c][0];
            Buffer& b = tex[c][1];
            Buffer& d = tex[c][2];
            const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{k.format, kW, kH, 1, kPitch, kTexBytes};
            const D3D12DDIARG_PLACED_RESOURCE placed{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint};
            const D3D12DDIARG_BUFFER_PLACEMENT from = at(upload, UINT64{c} * kCaseUp), ta = at(a, 0), tb = at(b, 0),
                                               td = at(d, 0);
            barrier(a, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
            t.pfnCopyTextureRegion(rec.hlist(), &ta, subresource, 0, 0, 0, &from, placed, nullptr);
            barrier(a, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            barrier(b, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(d, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            const FLOAT one_zero[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            t.pfnClearUnorderedAccessViewFloat(rec.hlist(), {visible_gpu.ptr + UINT64{2 * c} * increment},
                                               {plain_cpu.ptr + SIZE_T{2 * c} * increment}, b.hres(), one_zero, 0,
                                               nullptr);
            const UINT values[4] = {k.bytes == 4 ? 0x12345678u : 0x1234u, 0x5678u, 0x9ABCu, 0xDEF0u};
            t.pfnClearUnorderedAccessViewUint(rec.hlist(), {visible_gpu.ptr + UINT64{2 * c + 1} * increment},
                                              {plain_cpu.ptr + SIZE_T{2 * c + 1} * increment}, d.hres(), values, 0,
                                              nullptr);
            barrier(b, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_DEST);
            barrier(d, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_DEST);
            const D3D12DDI_BOX from_a{2, 0, 0, 6, 2, 1}, from_up{4, 1, 0, 8, 3, 1};
            t.pfnCopyTextureRegion(rec.hlist(), &tb, subresource, 2, 1, 0, &ta, subresource, &from_a);
            t.pfnCopyTextureRegion(rec.hlist(), &td, subresource, 2, 2, 0, &from, placed, &from_up);
            barrier(b, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            barrier(d, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            for (UINT i = 0; i < 3; ++i) {
                const D3D12DDIARG_BUFFER_PLACEMENT out = at(readback, UINT64{c} * kCaseDown + i * kTexBytes);
                const D3D12DDIARG_BUFFER_PLACEMENT src = at(tex[c][i], 0);
                t.pfnCopyTextureRegion(rec.hlist(), &out, placed, 0, 0, 0, &src, subresource, nullptr);
            }
        }
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        const uint32_t errors = device.shell.device_errors - errors_before;
        const uint32_t list_errors = device.shell.list_errors - list_errors_before;
        checkf(hr == S_OK && !errors && !list_errors,
               "stored formats: views, clears and copies of the six formats recorded and executed (hr %08lx, %u list "
               "errors, %u device errors)",
               static_cast<unsigned long>(hr), list_errors, errors);
        if (hr == S_OK && wait_queue_idle(env, queue, "stored formats") &&
            env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            const auto* got = static_cast<const BYTE*>(cpu);
            std::string bad;
            for (UINT c = 0; c < kCaseCount; ++c) {
                const Case& k = kCases[c];
                const UINT p = k.pixels;
                BYTE want[3][kTexBytes]{};
                expect(k, c, nullptr, 0, 0, 0, 0, kW / p, kH, want[0]);
                expect(k, c, k.float_clear, 2 / p, 1, 2 / p, 0, (6 + p - 1) / p - 2 / p, 2, want[1]);
                expect(k, c, k.uint_clear, 2 / p, 2, 4 / p, 1, 8 / p - 4 / p, 2, want[2]);
                for (UINT i = 0; i < 3; ++i) {
                    const UINT rows = mismatches(k, got + c * kCaseDown + i * kTexBytes, want[i]);
                    if (rows) {
                        char line[64];
                        std::snprintf(line, sizeof(line), " %u/%c:%u", static_cast<unsigned>(k.format), "ABC"[i],
                                      rows);
                        bad += line;
                    }
                }
            }
            checkf(bad.empty(),
                   "stored formats: AYUV, Y410, Y416, YUY2, Y210 and Y216 read back exactly: upload, clear through the "
                   "view of the video format and through the UINT view, copies with a box and an offset in pixels "
                   "(rows differ, format/texture:rows:%s)",
                   bad.empty() ? " none" : bad.c_str());
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    env.core.pfnDestroyDescriptorHeap(device.h(), hvisible);
    env.core.pfnDestroyDescriptorHeap(device.h(), hplain);
    for (auto& set : tex)
        for (Buffer& t : set) destroy_buffer(env, device, t);
    destroy_buffer(env, device, readback);
    destroy_buffer(env, device, upload);
}

} // namespace harness
