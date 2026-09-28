// SPDX-License-Identifier: MIT
// Round trip 4: device query slots the runtime may call around CreateDevice (which ones it calls is an INFERENCE,
// INTEGRATION.md). Each answer is compared with the engine's own answer to the same question.
#include "harness.h"
#include <cstring>

namespace harness {

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
