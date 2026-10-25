// PROVENANCE: compares against GPUOpen-LibrariesAndSDKs/display-library public headers (MIT).
// Compile-only gate: the public SDK and our subset must agree in both Windows ABIs.
#include <stddef.h>
namespace sdk {
#include "adl_structures.h"
}
#include "../adl-abi.h"
#define SAME(T) static_assert(sizeof(T) == sizeof(sdk::T)); static_assert(alignof(T) == alignof(sdk::T))
#define FIELD(T, F) static_assert(offsetof(T, F) == offsetof(sdk::T, F))
SAME(AdapterInfo); SAME(ADLMemoryInfo); SAME(ADLVersionsInfo); SAME(ADLVersionsInfoX2);
SAME(ADLDisplayID); SAME(ADLDisplayInfo); SAME(ADLMode); SAME(ADLDisplayMap); SAME(ADLDisplayTarget);
SAME(ADLSLSGrid); SAME(ADLSLSMap); SAME(ADLSLSTarget); SAME(ADLSLSMode); SAME(ADLBezelTransientMode); SAME(ADLSLSOffset);
FIELD(AdapterInfo, strUDID); FIELD(AdapterInfo, strPNPString); FIELD(AdapterInfo, strDriverPath);
FIELD(AdapterInfo, iOSDisplayIndex); FIELD(ADLMemoryInfo, iMemoryBandwidth);
FIELD(ADLVersionsInfoX2, strCrimsonVersion); FIELD(ADLVersionsInfoX2, strCatalystWebLink);
FIELD(ADLMode, iModeValue); FIELD(ADLDisplayInfo, iDisplayInfoValue); FIELD(ADLDisplayMap, iDisplayMapValue);
FIELD(ADLSLSMap, iSLSMapValue); FIELD(ADLSLSTarget, iSLSTargetValue); FIELD(ADLSLSMode, iSLSNativeModeValue);
FIELD(ADLBezelTransientMode, iSLSBezelTransientModeValue); FIELD(ADLSLSOffset, iBezelffsetValue);
