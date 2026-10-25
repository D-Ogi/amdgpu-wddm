// SPDX-License-Identifier: MIT
// raytracing-tier12.h against the headers that define what it names (build-engine-ddi.ps1, step 1): d3d12.h of
// Agility SDK 1.619.6 and d3d12umddi.h of WDK 10.0.28000. The shell builds with the 26100 headers, where the checks
// at the end of raytracing-tier12.h are inactive; this build is where they hold. Both guards must be active, so that
// a changed include path cannot turn the checks off without a failure here.
#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include "raytracing-tier12.h"
#include <cstdio>

#if !defined(D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT)
#error "d3d12.h of this build has no opacity micromaps: the API checks of raytracing-tier12.h are inactive"
#endif
#if !defined(D3D12DDI_BUILD_VERSION_0118)
#error "d3d12umddi.h of this build has no DDI 0118: the DDI checks of raytracing-tier12.h are inactive"
#endif

int main() {
    std::printf("PASSED: raytracing-tier12.h values and layouts against Agility 1.619.6 d3d12.h and WDK 28000 "
                "d3d12umddi.h (%u-bit)\n", static_cast<unsigned>(sizeof(void*) * 8));
    return 0;
}
