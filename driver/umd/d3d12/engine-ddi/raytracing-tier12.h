// SPDX-License-Identifier: MIT
// engine-ddi: the values and layouts of raytracing tier 1.2 (opacity micromaps) and shader model 6.9 that the
// headers of the build (WDK and SDK 10.0.26100) do not have. The shell reports tier 1.2 at its own negotiated DDI
// version (INTEGRATION.md, "Raytracing tier 1.2"), so it names the new members here instead of in the headers.
//
// H = d3d12umddi.h of WDK 10.0.28000 (DDI 0110 and 0118), A = d3d12.h of Agility SDK 1.619. The DDI and the API
// have the same values and the same layouts for all of these. A build with either header checks every item below
// against it (the static_asserts at the end of this file).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include <cstddef>
#include <cstring>

namespace engine_ddi::rt12 {

constexpr UINT kTier = 12;                        // D3D12_RAYTRACING_TIER_1_2 A:3013, DDI H:7708
constexpr UINT kShaderModel = 0x69;               // D3D_SHADER_MODEL_6_9 A:2868 (also in SDK 10.0.26100)
constexpr UINT kDdiShaderModel = 0x00060095;      // D3D12DDI_SHADER_MODEL_6_9_RELEASE_0118 H:3510
constexpr UINT kOmmArrayType = 2;                 // ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY A:15645, H:8115
constexpr UINT kOmmTrianglesType = 2;             // GEOMETRY_TYPE_OMM_TRIANGLES A:15508, H:7971
constexpr UINT kBuildAllowOmmUpdate = 0x40;       // BUILD_FLAG_ALLOW_OMM_UPDATE A:15626, H:8096
constexpr UINT kBuildAllowDisableOmms = 0x80;     // BUILD_FLAG_ALLOW_DISABLE_OMMS A:15627, H:8097
constexpr UINT kBuildAllowDataAccess = 0x100;     // BUILD_FLAG_ALLOW_DATA_ACCESS H:8098 (DDI only, shader model 6.10)
constexpr UINT kInstanceForceOmm2State = 0x10;    // INSTANCE_FLAG_FORCE_OMM_2_STATE A:15519, H:7981
constexpr UINT kInstanceDisableOmms = 0x20;       // INSTANCE_FLAG_DISABLE_OMMS A:15520, H:7982
constexpr UINT kPipelineAllowOmms = 0x400;        // PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS A:15353, H:7915
constexpr UINT kFormatOc1State2 = 1;              // OPACITY_MICROMAP_FORMAT_OC1_2_STATE A:15591, H:8050
constexpr UINT kFormatOc1State4 = 2;              // OPACITY_MICROMAP_FORMAT_OC1_4_STATE A:15592, H:8051
constexpr UINT kMaxSubdivisionLevel = 12;         // OPACITY_MICROMAP_OC1_MAX_SUBDIVISION_LEVEL A:1305

struct HistogramEntry {                           // A:15797-15802, H:8134-8139
    UINT Count;
    UINT SubdivisionLevel;
    UINT Format;
};
struct ArrayDesc {                                // A:15804-15810, H:8141-8147
    UINT NumOmmHistogramEntries;
    const HistogramEntry* pOmmHistogram;
    D3D12_GPU_VIRTUAL_ADDRESS InputBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE PerOmmDescs;
};
struct Linkage {                                  // A:15601-15607, H:8061-8067
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE OpacityMicromapIndexBuffer;
    DXGI_FORMAT OpacityMicromapIndexFormat;
    UINT OpacityMicromapBaseLocation;
    D3D12_GPU_VIRTUAL_ADDRESS OpacityMicromapArray;
};
struct OmmTriangles {                             // A:15609-15613, H:8069-8073
    const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC* pTriangles;
    const Linkage* pOmmLinkage;
};

// The new union members. An opacity micromap array's description is the fourth pointer of the inputs' union, beside
// pGeometryDescs (A:15812-15825, H:8149-8162). The OMM_TRIANGLES description is the third member of the geometry
// union, beside Triangles (A:15785-15795, H:8075-8085). Both work for the API and the DDI structures, which have
// the same members at the same offsets. They copy bytes, so no pointer of the wrong type reads the union.
template <class Inputs> const ArrayDesc* array_desc(const Inputs& in) noexcept {
    const ArrayDesc* d;
    std::memcpy(&d, &in.pGeometryDescs, sizeof(d));
    return d;
}
template <class Inputs> void set_array_desc(Inputs& in, const ArrayDesc* d) noexcept {
    static_assert(sizeof(in.pGeometryDescs) == sizeof(d), "a pointer of the inputs' union");
    std::memcpy(&in.pGeometryDescs, &d, sizeof(d));
}
template <class Geometry> OmmTriangles omm_triangles(const Geometry& g) noexcept {
    static_assert(sizeof(g.Triangles) >= sizeof(OmmTriangles), "OMM_TRIANGLES fits the geometry union");
    OmmTriangles t;
    std::memcpy(&t, &g.Triangles, sizeof(t));
    return t;
}
template <class Geometry> void set_omm_triangles(Geometry& g, const OmmTriangles& t) noexcept {
    std::memcpy(&g.Triangles, &t, sizeof(t));
}

static_assert(sizeof(HistogramEntry) == 12, "OMM histogram entry");
static_assert(sizeof(void*) == 8 ? sizeof(ArrayDesc) == 40 && sizeof(Linkage) == 32 && sizeof(OmmTriangles) == 16
                                 : sizeof(ArrayDesc) == 32 && sizeof(Linkage) == 32 && sizeof(OmmTriangles) == 8,
              "OMM array, linkage and triangles sizes on x64 and x86");

// Checks against the headers that have these items.
#if defined(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT)
static_assert(kTier == D3D12_RAYTRACING_TIER_1_2 && kShaderModel == D3D_SHADER_MODEL_6_9 &&
                  kOmmArrayType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY &&
                  kOmmTrianglesType == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES &&
                  kBuildAllowOmmUpdate == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE &&
                  kBuildAllowDisableOmms == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS &&
                  kInstanceForceOmm2State == D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE &&
                  kInstanceDisableOmms == D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS &&
                  kPipelineAllowOmms == D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS &&
                  kFormatOc1State2 == D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE &&
                  kFormatOc1State4 == D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE &&
                  kMaxSubdivisionLevel == D3D12_RAYTRACING_OPACITY_MICROMAP_OC1_MAX_SUBDIVISION_LEVEL,
              "API values");
static_assert(sizeof(HistogramEntry) == sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY) &&
                  offsetof(HistogramEntry, SubdivisionLevel) ==
                      offsetof(D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY, SubdivisionLevel) &&
                  offsetof(HistogramEntry, Format) ==
                      offsetof(D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY, Format),
              "API histogram entry");
static_assert(sizeof(ArrayDesc) == sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC) &&
                  offsetof(ArrayDesc, pOmmHistogram) ==
                      offsetof(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC, pOmmHistogram) &&
                  offsetof(ArrayDesc, InputBuffer) ==
                      offsetof(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC, InputBuffer) &&
                  offsetof(ArrayDesc, PerOmmDescs) ==
                      offsetof(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC, PerOmmDescs),
              "API array description");
static_assert(sizeof(Linkage) == sizeof(D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC) &&
                  offsetof(Linkage, OpacityMicromapIndexFormat) ==
                      offsetof(D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC, OpacityMicromapIndexFormat) &&
                  offsetof(Linkage, OpacityMicromapBaseLocation) ==
                      offsetof(D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC, OpacityMicromapBaseLocation) &&
                  offsetof(Linkage, OpacityMicromapArray) ==
                      offsetof(D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC, OpacityMicromapArray),
              "API linkage");
static_assert(sizeof(OmmTriangles) == sizeof(D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC) &&
                  offsetof(OmmTriangles, pOmmLinkage) ==
                      offsetof(D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC, pOmmLinkage) &&
                  offsetof(D3D12_RAYTRACING_GEOMETRY_DESC, OmmTriangles) ==
                      offsetof(D3D12_RAYTRACING_GEOMETRY_DESC, Triangles) &&
                  offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, pOpacityMicromapArrayDesc) ==
                      offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, pGeometryDescs),
              "API triangles and the union members");
#endif
#if defined(D3D12DDI_BUILD_VERSION_0118)
static_assert(kTier == D3D12DDI_RAYTRACING_TIER_1_2 && kDdiShaderModel == D3D12DDI_SHADER_MODEL_6_9_RELEASE_0118 &&
                  kOmmArrayType == D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY_0110 &&
                  kOmmTrianglesType == D3D12DDI_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES_0110 &&
                  kBuildAllowOmmUpdate == D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE_0110 &&
                  kBuildAllowDisableOmms ==
                      D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS_0110 &&
                  kBuildAllowDataAccess ==
                      D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DATA_ACCESS_0110 &&
                  kInstanceForceOmm2State == D3D12DDI_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE_0110 &&
                  kInstanceDisableOmms == D3D12DDI_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS_0110 &&
                  kPipelineAllowOmms == D3D12DDI_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS_0110 &&
                  kFormatOc1State2 == D3D12DDI_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE &&
                  kFormatOc1State4 == D3D12DDI_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE,
              "DDI values");
static_assert(sizeof(HistogramEntry) == sizeof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY_0110) &&
                  offsetof(HistogramEntry, SubdivisionLevel) ==
                      offsetof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY_0110, SubdivisionLevel) &&
                  offsetof(HistogramEntry, Format) ==
                      offsetof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY_0110, Format) &&
                  sizeof(ArrayDesc) == sizeof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC_0110) &&
                  offsetof(ArrayDesc, pOmmHistogram) ==
                      offsetof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC_0110, pOmmHistogram) &&
                  offsetof(ArrayDesc, InputBuffer) ==
                      offsetof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC_0110, InputBuffer) &&
                  offsetof(ArrayDesc, PerOmmDescs) ==
                      offsetof(D3D12DDI_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC_0110, PerOmmDescs),
              "DDI histogram entry and array description");
static_assert(sizeof(Linkage) == sizeof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC_0110) &&
                  offsetof(Linkage, OpacityMicromapIndexFormat) ==
                      offsetof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC_0110, OpacityMicromapIndexFormat) &&
                  offsetof(Linkage, OpacityMicromapBaseLocation) ==
                      offsetof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC_0110, OpacityMicromapBaseLocation) &&
                  offsetof(Linkage, OpacityMicromapArray) ==
                      offsetof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC_0110, OpacityMicromapArray) &&
                  sizeof(OmmTriangles) == sizeof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC_0110) &&
                  offsetof(OmmTriangles, pOmmLinkage) ==
                      offsetof(D3D12DDI_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC_0110, pOmmLinkage),
              "DDI linkage and triangles");
static_assert(offsetof(D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054, OMMTriangles) ==
                      offsetof(D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054, Triangles) &&
                  offsetof(D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054, pOpacityMicromapArrayDesc) ==
                      offsetof(D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054, pGeometryDescs),
              "DDI union members");
#endif

}  // namespace engine_ddi::rt12
