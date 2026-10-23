// SPDX-License-Identifier: MIT
// Round trip 4: device query slots the runtime may call around CreateDevice (which ones it calls is an INFERENCE,
// INTEGRATION.md). Each answer is compared with the engine's own answer to the same question.
#include "harness.h"
#include "format-list.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace harness {
namespace {

bool typeless_parent(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Both format slots over the whole DXGI_FORMAT enum and past its end, each format through CheckFormatSupport and
// then CheckMultisampleQualityLevels for 2..32 samples, flags 0: quality levels above 1 sample only for a format
// whose support answer carries MULTISAMPLE_RENDERTARGET (or a typeless parent, D3D11.3 functional spec 19.2.3), 4x
// and (below 128 bits) 8x for every such format (19.2.5), 1 level at 1 sample and 0 at 0 and 33 (WDK d3d10umddi
// pfnd3dwddm1_3ddi_checkmultisamplequalitylevels), and no device error for any value, known or not. NOT_SUPPORTED
// only for R10G10B10_XR_BIAS_A2_UNORM, as its only bit, and exactly while the engine makes no 2D texture of it;
// UAV_READS only with UAV_WRITES and never on a video format.
void test_format_walk(Env& env, Device& device) {
    const uint32_t errors_before = device.shell.device_errors;
    unsigned formats = 0, supported = 0, msaa_rt = 0, bad_levels = 0, bad_required = 0, bad_edges = 0;
    unsigned bad_allowed = 0, bad_unsupported = 0, bad_reads = 0, short_formats = 0;
    // D3D11 video processing and decode have no counterpart in this DDI: engine-ddi reports none of these bits.
    constexpr UINT kVideoBits = D3D12DDI_FORMAT_SUPPORT_DECODER_OUTPUT | D3D12DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT |
                                D3D12DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
    auto levels_of = [&](UINT f, UINT n) {
        UINT levels = 0xCDCDCDCDu;
        env.core.pfnCheckMultisampleQualityLevels(device.h(), static_cast<DXGI_FORMAT>(f), n,
                                                  D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_NONE, &levels);
        return levels;
    };
    for (UINT f = 0; f <= 300; ++f) {
        const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(f);
        UINT bits = 0xCDCDCDCDu;
        env.core.pfnCheckFormatSupport(device.h(), format, &bits);
        ++formats;
        if (bits == 0xCDCDCDCDu) {
            checkf(false, "format walk: CheckFormatSupport(%u) left its output unwritten", f);
            continue;
        }
        // NOT_SUPPORTED is the one answer outside the format list (d3d12umddi.h); the format that may carry it is
        // checked against the engine after the walk.
        if (bits & D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED) {
            if ((format != DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM || bits != D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED) &&
                !bad_unsupported++)
                checkf(false, "format walk: format %u: support %#x carries NOT_SUPPORTED", f, bits);
            bits &= ~static_cast<UINT>(D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED);
        }
        if (bits) ++supported;
        const engine_ddi::FormatListEntry* entry = nullptr;
        for (const engine_ddi::FormatListEntry& e : engine_ddi::kFormatList)
            if (e.format == format) entry = &e;
        const UINT allowed = entry ? entry->allowed : 0;
        if (bits & ~allowed) {
            if (!bad_allowed++)
                checkf(false, "format walk: format %u: support %#x has bits %#x the format list does not allow", f,
                       bits, bits & ~allowed);
        }
        // Typed UAV loads only with typed UAV stores, and never on a video format, not even one the engine loads
        // (NV12, P010 and P016 on the lab, 266): DirectX-Specs d3d/UAVTypedLoad.md.
        const bool video = (format >= DXGI_FORMAT_AYUV && format <= DXGI_FORMAT_A8P8) ||
                           (format >= DXGI_FORMAT_P208 && format <= DXGI_FORMAT_V408);
        if ((bits & D3D12DDI_FORMAT_SUPPORT_UAV_READS) && (video || !(bits & D3D12DDI_FORMAT_SUPPORT_UAV_WRITES))) {
            if (!bad_reads++)
                checkf(false, "format walk: format %u: support %#x has UAV_READS %s", f, bits,
                       video ? "on a video format" : "without UAV_WRITES");
        }
        const UINT missing = entry ? entry->required & ~bits & ~kVideoBits : 0;
        if (missing) {
            ++short_formats;
            std::printf("note  format walk: format %u lacks format-list required bits %#x (support %#x)\n", f, missing,
                        bits);
        }
        const bool rt = (bits & D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) != 0;
        msaa_rt += rt;
        for (UINT n = 2; n <= 32; ++n) {
            const UINT levels = levels_of(f, n);
            if (levels == 0xCDCDCDCDu || (levels && !rt && !typeless_parent(format))) {
                if (!bad_levels++)
                    checkf(false, "format walk: format %u x%u: %u levels, support %#x has no MULTISAMPLE_RENDERTARGET",
                           f, n, levels, bits);
            }
        }
        const bool wide = format >= DXGI_FORMAT_R32G32B32A32_TYPELESS && format <= DXGI_FORMAT_R32G32B32A32_SINT;
        if (rt && (!levels_of(f, 4) || (!wide && !levels_of(f, 8)))) {
            if (!bad_required++)
                checkf(false, "format walk: format %u reports MULTISAMPLE_RENDERTARGET with x4 %u, x8 %u levels", f,
                       levels_of(f, 4), levels_of(f, 8));
        }
        if (levels_of(f, 1) != 1 || levels_of(f, 0) != 0 || levels_of(f, 33) != 0) {
            if (!bad_edges++)
                checkf(false, "format walk: format %u: %u levels at x1, %u at x0, %u at x33 (want 1, 0, 0)", f,
                       levels_of(f, 1), levels_of(f, 0), levels_of(f, 33));
        }
    }
    UINT bits = 0xCDCDCDCDu;
    env.core.pfnCheckFormatSupport(device.h(), static_cast<DXGI_FORMAT>(0xFFFFFFFFu), &bits);
    checkf(bits == 0 && levels_of(0xFFFFFFFFu, 4) == 0, "format walk: format 0xFFFFFFFF: support %#x, x4 %u levels",
           bits, levels_of(0xFFFFFFFFu, 4));
    checkf(!bad_levels && !bad_required && !bad_edges && !bad_allowed && !bad_unsupported && !bad_reads,
           "format walk: %u values, %u with support bits, %u multisample targets; %u MSAA mismatches, %u missing "
           "x4/x8, %u wrong x0/x1/x33 answers, %u bits outside the format list, %u misplaced NOT_SUPPORTED, %u "
           "misplaced UAV_READS",
           formats, supported, msaa_rt, bad_levels, bad_required, bad_edges, bad_allowed, bad_unsupported, bad_reads);
    // R10G10B10_XR_BIAS_A2_UNORM: answered 0, the runtime still offered it as a display format (266).
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT s{DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM};
        const bool texture = SUCCEEDED(env.engine->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s))) &&
                             (s.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D);
        UINT support = 0xCDCDCDCDu;
        env.core.pfnCheckFormatSupport(device.h(), DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, &support);
        checkf(texture ? !(support & D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED)
                       : support == D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED,
               "format walk: R10G10B10_XR_BIAS_A2_UNORM: support %#x while the engine makes %s 2D texture of it",
               support, texture ? "a" : "no");
    }
    for (DXGI_FORMAT f : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT}) {
        UINT support = 0;
        env.core.pfnCheckFormatSupport(device.h(), f, &support);
        checkf((support & (D3D12DDI_FORMAT_SUPPORT_UAV_READS | D3D12DDI_FORMAT_SUPPORT_UAV_WRITES)) ==
                   (D3D12DDI_FORMAT_SUPPORT_UAV_READS | D3D12DDI_FORMAT_SUPPORT_UAV_WRITES),
               "format walk: format %u carries the mandatory typed UAV load and store (support %#x)",
               static_cast<unsigned>(f), support);
    }
    // TypedUAVLoadAdditionalFormats is all or nothing over this set (D3D12 typed UAV loads): the engine's cap decides
    // whether every one of them carries UAV_READS.
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
        unsigned wrong = 0;
        for (DXGI_FORMAT f :
             {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT,
              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT,
              DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R16_FLOAT,
              DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UINT,
              DXGI_FORMAT_R8_SINT}) {
            UINT support = 0;
            env.core.pfnCheckFormatSupport(device.h(), f, &support);
            wrong += ((support & D3D12DDI_FORMAT_SUPPORT_UAV_READS) != 0) != !!options.TypedUAVLoadAdditionalFormats;
        }
        checkf(!wrong, "format walk: UAV_READS on the additional-formats set follows TypedUAVLoadAdditionalFormats %d "
                       "(%u formats differ)", options.TypedUAVLoadAdditionalFormats, wrong);
    } else {
        checkf(false, "format walk: the engine answers D3D12_OPTIONS");
    }
    std::printf("note  format walk: %u listed formats lack a non-video bit the FL11_1 format list requires\n",
                short_formats);
    // Support and size agree: every format whose answer carries a bit only a texture can have gets a size for a
    // 64 x 64 2D texture from CheckResourceAllocationInfo, not UINT64_MAX. The runtime adds the FL11_1 required
    // support of a format to whatever the driver answers (YUY2 answered 0 came back with TEXTURE2D and SHADER_SAMPLE,
    // R8G8_B8G8_UNORM was not asked at all, 284), so the listed formats with required texture bits that get no size
    // are printed: the runtime offers them and creation refuses them.
    {
        constexpr UINT kTextureBits =
            D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER |
            D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_BLENDABLE |
            D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
            D3D12DDI_FORMAT_SUPPORT_DISPLAY;
        auto sized = [&](DXGI_FORMAT f) {
            D3D12DDIARG_CREATERESOURCE_0088 res{};
            res.ResourceType = D3D12DDI_RT_TEXTURE2D;
            res.Width = 64;
            res.Height = 64;
            res.DepthOrArraySize = 1;
            res.MipLevels = 1;
            res.Format = f;
            res.SampleDesc = {1, 0};
            res.Layout = D3D12DDI_TL_UNDEFINED;
            res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
            D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
            env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1,
                                                    &info);
            return info.ResourceDataSize && info.ResourceDataSize != UINT64_MAX;
        };
        unsigned claimed = 0;
        std::string unsized, gap;
        for (UINT f = 0; f <= 300; ++f) {
            const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(f);
            UINT support = 0;
            env.core.pfnCheckFormatSupport(device.h(), format, &support);
            if ((support & ~static_cast<UINT>(D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED)) & kTextureBits) {
                ++claimed;
                if (!sized(format)) unsized += " " + std::to_string(f);
            }
        }
        for (const engine_ddi::FormatListEntry& e : engine_ddi::kFormatList) {
            if ((e.required & kTextureBits) && !sized(e.format))
                gap += " " + std::to_string(static_cast<unsigned>(e.format));
        }
        checkf(claimed && unsized.empty(),
               "format walk: each of the %u formats answered with a texture-only bit gets a size for a 64 x 64 2D "
               "texture (no size:%s)",
               claimed, unsized.empty() ? " none" : unsized.c_str());
        std::printf("note  format walk: FL11_1 requires texture support the engine cannot size for formats:%s\n",
                    gap.empty() ? " none" : gap.c_str());
    }
    // The diagnostic line of CheckFormatSupport: one for each value below 256 and one for every larger value together,
    // however often the walk asked; R8G8B8A8_UNORM's carries the engine's raw answer next to the DDI answer.
    {
        unsigned wrong = 0;
        for (UINT f = 0; f < 256; ++f) {
            char prefix[48];
            std::snprintf(prefix, sizeof(prefix), "CheckFormatSupport: format %u:", f);
            wrong += refusal_lines(prefix).size() != 1 ? 1u : 0u;
        }
        const char kPrefix[] = "CheckFormatSupport: format ";
        size_t beyond = 0;
        for (const std::string& line : refusal_lines(kPrefix))
            beyond += std::strtoull(line.c_str() + sizeof(kPrefix) - 1, nullptr, 10) >= 256 ? 1 : 0;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT s{DXGI_FORMAT_R8G8B8A8_UNORM};
        const HRESULT hr = env.engine->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s));
        UINT answer = 0;
        env.core.pfnCheckFormatSupport(device.h(), DXGI_FORMAT_R8G8B8A8_UNORM, &answer);
        char want[160];
        std::snprintf(want, sizeof(want), "CheckFormatSupport: format %u: engine %08lx, Support1 %#x, Support2 %#x; "
                      "answer %#x", static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM), static_cast<unsigned long>(hr),
                      static_cast<unsigned>(s.Support1), static_cast<unsigned>(s.Support2), answer);
        const std::vector<std::string> rgba = refusal_lines("CheckFormatSupport: format 28:");
        checkf(!wrong && beyond == 1 && rgba.size() == 1 && rgba[0] == want,
               "format walk: one CheckFormatSupport line per format value below 256 (%u values differ) and one for "
               "all others (%zu); R8G8B8A8_UNORM's is \"%s\"",
               wrong, beyond, rgba.empty() ? "" : rgba[0].c_str());
    }
    checkf(device.shell.device_errors == errors_before, "format walk: no device error reported (%u new)",
           device.shell.device_errors - errors_before);
}

// Castable formats (DDI 0094 review C1 and C2). A BC5_UNORM 256 x 256 image with unordered access is valid only with
// an uncompressed castable format, so the engine sizes it only through GetResourceAllocationInfo3. Lists of 3, 8, 9
// and 12 formats are created committed (12 also placed at offset 0 of the first, and reserved when the engine has
// sparse BC5), the caller's array is overwritten and freed, and CheckExistingResourceAllocationInfo must still give
// the engine's answer for the whole list, with each format kept by the record and no legacy query. A failed copy
// fails the create with nothing left behind. Without an ID3D12Device12 a list is unsized and never reaches the
// legacy query; a description without a list still does (the positive control).
D3D12DDIARG_CREATERESOURCE_0088 castable_texture(const DXGI_FORMAT* formats, UINT count, bool reserved) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = 256;
    res.Height = 256;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_BC5_UNORM;
    res.SampleDesc = {1, 0};
    res.Layout = reserved ? D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE : D3D12DDI_TL_UNDEFINED;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    res.NumCastableFormats = count;
    res.pCastableFormats = formats;
    return res;
}

// kind 0 committed, 1 placed at offset 0 of base's memory, 2 reserved.
HRESULT create_castable(Env& env, Device& device, const D3D12DDIARG_CREATERESOURCE_0088& in, int kind,
                        const Buffer* base, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res = in;
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    D3D12DDIARG_CREATEHEAP_0001* heap_desc = nullptr;
    if (kind == 0) {
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
        env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
        const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
        heap.ByteSize = info.ResourceDataSize;
        heap.Alignment = info.ResourceDataAlignment;
        heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
        heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
        heap.Flags = D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        heap_desc = &heap;
    } else if (kind == 1) {
        res.ReuseBufferGPUVA.BaseAddress.UMD = {base->hres(), 0};
    }
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), heap_desc, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    if (heap_desc) out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if ((heap_desc && !out.heap) || !out.resource) return E_OUTOFMEMORY;
    const HRESULT hr = env.core.pfnCreateHeapAndResource(device.h(), heap_desc, out.hheap(),
                                                         D3D12DDI_HRTRESOURCE{&out.rt}, &res, nullptr,
                                                         D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
    if (FAILED(hr)) out.heap = out.resource = nullptr;  // nothing was constructed: nothing to destroy
    return hr;
}

void test_castable_sizing(Env& env, Device& device) {
    ID3D12Device12* d12 = nullptr;
    if (FAILED(env.engine->QueryInterface(__uuidof(ID3D12Device12), reinterpret_cast<void**>(&d12))) || !d12) {
        checkf(false, "castable: the engine has no ID3D12Device12");
        return;
    }
    const DXGI_FORMAT pattern[] = {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_R32G32B32A32_UINT,
                                   DXGI_FORMAT_R32G32B32A32_SINT};
    std::vector<DXGI_FORMAT> all(12);
    for (size_t i = 0; i < all.size(); ++i) all[i] = pattern[i % 4];
    // The engine's answer for a description and a list, as allocation_info asks it (the record's alignment first).
    auto expect = [&](D3D12_RESOURCE_DESC1 desc, const DXGI_FORMAT* formats, UINT32 count) {
        D3D12_RESOURCE_ALLOCATION_INFO info = d12->GetResourceAllocationInfo3(0, 1, &desc, &count, &formats, nullptr);
        if (info.SizeInBytes == UINT64_MAX && desc.Alignment) {
            desc.Alignment = 0;
            info = d12->GetResourceAllocationInfo3(0, 1, &desc, &count, &formats, nullptr);
        }
        return info;
    };
    const uint32_t errors_before = device.shell.device_errors;
    const int64_t copies_before = engine_ddi::harness_live_castable_copies();

    // The query of a create: the three-format shape of the first game that needed it.
    {
        const D3D12DDIARG_CREATERESOURCE_0088 res = castable_texture(all.data(), 3, false);
        D3D12_RESOURCE_DESC1 desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, DXGI_FORMAT_BC5_UNORM, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const D3D12_RESOURCE_ALLOCATION_INFO want = expect(desc, all.data(), 3);
        const uint64_t legacy = engine_ddi::harness_legacy_sizing_calls();
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
        env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        checkf(want.SizeInBytes != UINT64_MAX && info.ResourceDataSize == want.SizeInBytes &&
                   info.ResourceDataAlignment == want.Alignment &&
                   engine_ddi::harness_legacy_sizing_calls() == legacy,
               "castable: CheckResourceAllocationInfo BC5 UAV with 3 castable formats: %llu aligned %u, the engine's "
               "%llu aligned %llu, legacy queries %llu",
               static_cast<unsigned long long>(info.ResourceDataSize), info.ResourceDataAlignment,
               static_cast<unsigned long long>(want.SizeInBytes), static_cast<unsigned long long>(want.Alignment),
               static_cast<unsigned long long>(engine_ddi::harness_legacy_sizing_calls() - legacy));
    }

    // Creates, then the existing-resource query after the caller's array is gone.
    struct Made {
        Buffer buffer;
        std::vector<DXGI_FORMAT> formats;               // what the create got, kept by the test for the comparison
        const char* what;
    };
    std::vector<Made> made;
    Buffer base;
    auto create_one = [&](UINT count, int kind, const char* what) {
        auto* caller = new std::vector<DXGI_FORMAT>(all.begin(), all.begin() + count);
        Made m{Buffer{}, *caller, what};
        const HRESULT hr =
            create_castable(env, device, castable_texture(caller->data(), count, kind == 2), kind, &base, m.buffer);
        std::fill(caller->begin(), caller->end(), DXGI_FORMAT_UNKNOWN);
        delete caller;
        if (hr == S_OK) made.push_back(m);
        return hr;
    };
    HRESULT hr = create_one(12, 0, "committed 12");
    if (hr == S_OK) base = made.back().buffer;
    checkf(hr == S_OK, "castable: committed BC5 UAV image with 12 castable formats (hr %08lx)",
           static_cast<unsigned long>(hr));
    for (const UINT count : {3u, 8u, 9u}) {
        hr = create_one(count, 0, "committed");
        checkf(hr == S_OK, "castable: committed BC5 UAV image with %u castable formats (hr %08lx)", count,
               static_cast<unsigned long>(hr));
    }
    if (base.resource) {
        hr = create_one(9, 1, "placed 9");
        checkf(hr == S_OK, "castable: placed BC5 UAV image with 9 castable formats at offset 0 (hr %08lx)",
               static_cast<unsigned long>(hr));
    }
    const size_t before_reserved = made.size();
    const int64_t copies_reserved = engine_ddi::harness_live_castable_copies();
    hr = create_one(12, 2, "reserved 12");
    if (FAILED(hr)) {
        // A host GPU without sparse BC5: the create's failure must leave no copy behind.
        checkf(engine_ddi::harness_live_castable_copies() == copies_reserved && made.size() == before_reserved,
               "castable: reserved BC5 UAV image refused by this engine (hr %08lx) leaves no castable copy",
               static_cast<unsigned long>(hr));
        std::printf("castable: reserved BC5 image not available on this engine (hr %08lx); its unwind was checked\n",
                    static_cast<unsigned long>(hr));
    }
    for (const Made& m : made) {
        auto* record = static_cast<engine_ddi::ResourceRecord*>(m.buffer.resource);
        bool kept = record->castable_count == m.formats.size() && record->castable;
        for (size_t i = 0; kept && i < m.formats.size(); ++i) kept = record->castable[i] == m.formats[i];
        const D3D12_RESOURCE_ALLOCATION_INFO want =
            expect(record->desc, m.formats.data(), static_cast<UINT32>(m.formats.size()));
        const uint64_t legacy = engine_ddi::harness_legacy_sizing_calls();
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info;
        std::memset(&info, 0xCD, sizeof(info));
        env.core.pfnCheckExistingResourceAllocationInfo(device.h(), m.buffer.hres(), &info);
        checkf(kept && want.SizeInBytes != UINT64_MAX && info.ResourceDataSize == want.SizeInBytes &&
                   info.ResourceDataAlignment == want.Alignment &&
                   engine_ddi::harness_legacy_sizing_calls() == legacy,
               "castable: CheckExistingResourceAllocationInfo, %s (%zu formats, kept %d): %llu aligned %u, the "
               "engine's %llu aligned %llu, legacy queries %llu",
               m.what, m.formats.size(), kept ? 1 : 0, static_cast<unsigned long long>(info.ResourceDataSize),
               info.ResourceDataAlignment, static_cast<unsigned long long>(want.SizeInBytes),
               static_cast<unsigned long long>(want.Alignment),
               static_cast<unsigned long long>(engine_ddi::harness_legacy_sizing_calls() - legacy));
    }

    // A copy that cannot be made fails the create, with no record, no engine object and no copy left.
    {
        const uint32_t live = engine_ddi::harness_live_objects(device.context);
        const int64_t copies = engine_ddi::harness_live_castable_copies();
        engine_ddi::harness_fail_next_castable_copy();
        Buffer failed;
        hr = create_castable(env, device, castable_texture(all.data(), 12, false), 0, nullptr, failed);
        checkf(hr == E_OUTOFMEMORY && engine_ddi::harness_live_objects(device.context) == live &&
                   engine_ddi::harness_live_castable_copies() == copies,
               "castable: a failed copy of 12 formats fails the committed create with E_OUTOFMEMORY (hr %08lx), "
               "objects %u -> %u, copies %lld -> %lld",
               static_cast<unsigned long>(hr), live, engine_ddi::harness_live_objects(device.context),
               static_cast<long long>(copies),
               static_cast<long long>(engine_ddi::harness_live_castable_copies()));
    }

    // Destroy: placed and reserved first, the base last; every copy freed once.
    for (size_t i = made.size(); i-- > 0;) destroy_buffer(env, device, made[i].buffer);
    checkf(engine_ddi::harness_live_castable_copies() == copies_before,
           "castable: every record copy is freed with its resource (%lld left)",
           static_cast<long long>(engine_ddi::harness_live_castable_copies() - copies_before));

    // C2: without ID3D12Device12 a list is unsized and the legacy query is not asked; without a list it is.
    {
        ID3D12Device12* saved = device.context->device12;
        device.context->device12 = nullptr;
        const uint64_t legacy = engine_ddi::harness_legacy_sizing_calls();
        const D3D12DDIARG_CREATERESOURCE_0088 res = castable_texture(all.data(), 3, false);
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
        env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        const uint64_t after_list = engine_ddi::harness_legacy_sizing_calls();
        D3D12DDIARG_CREATERESOURCE_0088 plain = castable_texture(nullptr, 0, false);
        plain.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 control{};
        env.core.pfnCheckResourceAllocationInfo(device.h(), &plain, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1,
                                                &control);
        const uint64_t after_plain = engine_ddi::harness_legacy_sizing_calls();
        device.context->device12 = saved;
        checkf(info.ResourceDataSize == UINT64_MAX && after_list == legacy && control.ResourceDataSize &&
                   control.ResourceDataSize != UINT64_MAX && after_plain > after_list,
               "castable: without ID3D12Device12, 3 castable formats answer UINT64_MAX (%llu) with no legacy query "
               "(%llu), and a description without a list is sized (%llu) by it (%llu)",
               static_cast<unsigned long long>(info.ResourceDataSize),
               static_cast<unsigned long long>(after_list - legacy),
               static_cast<unsigned long long>(control.ResourceDataSize),
               static_cast<unsigned long long>(after_plain - after_list));
    }
    checkf(device.shell.device_errors == errors_before, "castable: no device error reported (%u)",
           device.shell.device_errors - errors_before);
    d12->Release();
}

} // namespace

void test_device_queries(Env& env, Device& device) {
    const uint32_t errors_before = device.shell.device_errors;

    // CheckMultisampleQualityLevels: the engine's D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, 1:1.
    for (UINT samples : {1u, 4u}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{DXGI_FORMAT_R8G8B8A8_UNORM, samples,
                                                        D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        const HRESULT hr = env.engine->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q));
        UINT levels = 0xCDCDCDCDu;
        env.core.pfnCheckMultisampleQualityLevels(device.h(), DXGI_FORMAT_R8G8B8A8_UNORM, samples,
                                                  D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_NONE, &levels);
        checkf(SUCCEEDED(hr) && levels == q.NumQualityLevels && (samples != 1 || levels == 1),
               "CheckMultisampleQualityLevels R8G8B8A8_UNORM x%u: %u levels, the engine's answer %u", samples, levels,
               q.NumQualityLevels);
    }

    test_format_walk(env, device);
    test_castable_sizing(env, device);

    // CheckExistingResourceAllocationInfo: the engine's size and alignment for the buffer's description.
    Buffer buffer;
    HRESULT hr = create_buffer(env, device, HeapKind::Default, 65536, true, buffer);
    if (hr == S_OK) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 65536;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_RESOURCE_ALLOCATION_INFO expect = env.engine->GetResourceAllocationInfo(0, 1, &desc);
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info;
        std::memset(&info, 0xCD, sizeof(info));
        env.core.pfnCheckExistingResourceAllocationInfo(device.h(), buffer.hres(), &info);
        checkf(info.ResourceDataSize == expect.SizeInBytes && info.ResourceDataAlignment == expect.Alignment &&
                   info.Layout == D3D12DDI_TL_ROW_MAJOR && !info.AdditionalDataSize && !info.AdditionalDataHeaderSize &&
                   info.AdditionalDataHeaderAlignment == info.ResourceDataAlignment &&
                   info.AdditionalDataAlignment == info.ResourceDataAlignment,
               "CheckExistingResourceAllocationInfo, 64 KiB UAV buffer: %llu bytes aligned %u, row major, no additional "
               "data but its alignments the resource's (%u, %u; a zero wraps the runtime's answer), the engine's "
               "%llu aligned %llu",
               static_cast<unsigned long long>(info.ResourceDataSize), info.ResourceDataAlignment,
               info.AdditionalDataHeaderAlignment, info.AdditionalDataAlignment,
               static_cast<unsigned long long>(expect.SizeInBytes), static_cast<unsigned long long>(expect.Alignment));

        // CheckSubresourceInfo: a buffer is one unswizzled row from offset 0.
        D3D12DDI_SUBRESOURCE_INFO sub;
        std::memset(&sub, 0xCD, sizeof(sub));
        const uint32_t before_sub = device.shell.device_errors;
        env.core.pfnCheckSubresourceInfo(device.h(), buffer.hres(), 0, &sub);
        checkf(!sub.Offset && sub.RowStride == 65536 && sub.DepthStride == 65536 && !sub.RowBytePreSwizzleOffset &&
                   !sub.ColumnPreSwizzleOffset && !sub.DepthPreSwizzleOffset &&
                   device.shell.device_errors == before_sub,
               "CheckSubresourceInfo, 64 KiB buffer: offset %llu, strides %llu and %llu, no device error",
               static_cast<unsigned long long>(sub.Offset), static_cast<unsigned long long>(sub.RowStride),
               static_cast<unsigned long long>(sub.DepthStride));
        destroy_buffer(env, device, buffer);
    } else {
        checkf(false, "CheckExistingResourceAllocationInfo: DEFAULT buffer (hr %08lx)", static_cast<unsigned long>(hr));
    }

    // ImplicitShaderCacheControl: no driver-managed cache is reported, so every control is a no-op.
    env.core.pfnImplicitShaderCacheControl(device.h(), D3D12DDI_IMPLICIT_SHADER_CACHE_CONTROL_FLAG_0080_CLEAR);
    checkf(device.shell.device_errors == errors_before, "device queries: no device error reported (%u new)",
           device.shell.device_errors - errors_before);
}

} // namespace harness
