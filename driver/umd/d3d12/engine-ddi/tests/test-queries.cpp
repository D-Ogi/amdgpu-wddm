// SPDX-License-Identifier: MIT
// Round trip 4: device query slots the runtime may call around CreateDevice (which ones it calls is an INFERENCE,
// INTEGRATION.md). Each answer is compared with the engine's own answer to the same question.
#include "harness.h"
#include <cstring>

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
// pfnd3dwddm1_3ddi_checkmultisamplequalitylevels), and no device error for any value, known or not.
void test_format_walk(Env& env, Device& device) {
    const uint32_t errors_before = device.shell.device_errors;
    unsigned formats = 0, supported = 0, msaa_rt = 0, bad_levels = 0, bad_required = 0, bad_edges = 0;
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
        if (bits) ++supported;
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
    checkf(!bad_levels && !bad_required && !bad_edges,
           "format walk: %u values, %u with support bits, %u multisample targets; %u MSAA mismatches, %u missing "
           "x4/x8, %u wrong x0/x1/x33 answers",
           formats, supported, msaa_rt, bad_levels, bad_required, bad_edges);
    checkf(device.shell.device_errors == errors_before, "format walk: no device error reported (%u new)",
           device.shell.device_errors - errors_before);
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
                   info.Layout == D3D12DDI_TL_ROW_MAJOR && !info.AdditionalDataSize && !info.AdditionalDataHeaderSize,
               "CheckExistingResourceAllocationInfo, 64 KiB UAV buffer: %llu bytes aligned %u, row major, the engine's "
               "%llu aligned %llu",
               static_cast<unsigned long long>(info.ResourceDataSize), info.ResourceDataAlignment,
               static_cast<unsigned long long>(expect.SizeInBytes), static_cast<unsigned long long>(expect.Alignment));
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
