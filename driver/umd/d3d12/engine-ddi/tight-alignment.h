// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <algorithm>
#include <cstdint>

namespace engine_ddi::tight {
// DirectX-Specs/d3d/D3D12TightPlacedResourceAlignment.md. The newer flag is outside
// the SDK 26100 C++ enum range, so modern builds must use the public Agility API header.
#if D3D12_SDK_VERSION >= 618
constexpr auto feature = D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT;
constexpr auto flag = D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT;
using Support = D3D12_FEATURE_DATA_TIGHT_ALIGNMENT;
static_assert(sizeof(Support) == 4);
inline bool enabled(const D3D12_RESOURCE_DESC1& d) noexcept { return (d.Flags & flag) != 0; }
inline bool supported(ID3D12Device* device) noexcept {
    Support s{};
    return device && SUCCEEDED(device->CheckFeatureSupport(feature, &s, sizeof(s))) && s.SupportTier == D3D12_TIGHT_ALIGNMENT_TIER_1;
}
inline bool layout_valid(const D3D12_RESOURCE_DESC1& d) noexcept {
    return d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN || d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
}
// The API combines allocations and rounds their sizes. The single-resource DDI reports buffer Width
// exactly. AlignmentRestriction is a floor, including the runtime's 4 KiB committed-resource floor.
inline bool normalize(const D3D12_RESOURCE_DESC1& d, UINT64 floor, D3D12_RESOURCE_ALLOCATION_INFO& info) noexcept {
    if (!enabled(d)) return true;
    if (!layout_valid(d) || !info.Alignment || info.SizeInBytes == UINT64_MAX ||
        (floor && (floor & (floor - 1)))) return false;
    const bool buffer = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const bool cross = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
    const UINT64 maximum = d.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
        : buffer && !cross ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if (floor > maximum || (buffer && !cross && info.Alignment > 256)) return false;
    info.Alignment = std::max<UINT64>(info.Alignment, std::max<UINT64>(floor, 8));
    if (cross) info.Alignment = std::max<UINT64>(info.Alignment, maximum);
    if (info.Alignment > maximum) return false;
    if (buffer) info.SizeInBytes = d.Width;
    return true;
}
#else
#if defined(D3D12DDI_BUILD_VERSION_0119)
#error DDI ladder tight alignment requires an Agility API header with D3D12_SDK_VERSION >= 618.
#endif
inline bool enabled(const D3D12_RESOURCE_DESC1&) noexcept { return false; }
inline bool normalize(const D3D12_RESOURCE_DESC1&, UINT64, D3D12_RESOURCE_ALLOCATION_INFO&) noexcept { return true; }
#endif
} // namespace engine_ddi::tight
