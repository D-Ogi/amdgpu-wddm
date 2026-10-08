// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../contract/amdgpu_wddm_surface_format.h"
#include "../../contract/bc250_scanout_caps.h"
#include "../../contract/bc250_shared_surface.h"
#include "../../kmd/gdi_private.h"
namespace bc250::umd {
// The D3D11 numbers the shared-surface wire format carries are the SDK's. D3D11_TEXTURE_LAYOUT is
// declared by d3d11_3.h, which this header does not need; ddi-resource.cpp checks that one where it
// builds the texture description.
static_assert(BC250_SHARED_BIND_SHADER_RESOURCE==D3D11_BIND_SHADER_RESOURCE &&
              BC250_SHARED_BIND_RENDER_TARGET==D3D11_BIND_RENDER_TARGET &&
              BC250_SHARED_BIND_UNORDERED_ACCESS==D3D11_BIND_UNORDERED_ACCESS &&
              BC250_SHARED_MISC_GENERATE_MIPS==D3D11_RESOURCE_MISC_GENERATE_MIPS &&
              BC250_SHARED_MISC_RESOURCE_CLAMP==D3D11_RESOURCE_MISC_RESOURCE_CLAMP &&
              BC250_SHARED_USAGE_DEFAULT==D3D11_USAGE_DEFAULT);
// The surface format table's numbers are the SDK's and the WDK's.
static_assert(AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT==DXGI_FORMAT_R16G16B16A16_FLOAT &&
              AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM==DXGI_FORMAT_R10G10B10A2_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM==DXGI_FORMAT_R8G8B8A8_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
              AMDGPU_WDDM_DXGI_A8_UNORM==DXGI_FORMAT_A8_UNORM &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM==DXGI_FORMAT_B8G8R8A8_UNORM &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
static_assert(AMDGPU_WDDM_D3DDDI_A8R8G8B8==D3DDDIFMT_A8R8G8B8 && AMDGPU_WDDM_D3DDDI_X8R8G8B8==D3DDDIFMT_X8R8G8B8 &&
              AMDGPU_WDDM_D3DDDI_A8==D3DDDIFMT_A8 && AMDGPU_WDDM_D3DDDI_A2B10G10R10==D3DDDIFMT_A2B10G10R10 &&
              AMDGPU_WDDM_D3DDDI_A8B8G8R8==D3DDDIFMT_A8B8G8R8 &&
              AMDGPU_WDDM_D3DDDI_A16B16G16R16F==D3DDDIFMT_A16B16G16R16F);
// The storage formats of a runtime surface (a swap-chain buffer, a shared texture such as a
// DirectComposition atlas): the table's COMPOSED rows, the set the kernel driver creates and the
// compositor's UMD opens. A row's sRGB view shares its storage and its LB7A format.
inline const AMDGPU_WDDM_SURFACE_FORMAT *runtime_surface_format(DXGI_FORMAT format) {
    unsigned count=0;
    const auto *rows=amdgpu_wddm_surface_formats(&count);
    for (unsigned i=0;i<count;++i)
        if (rows[i].dxgi && (UINT(format)==rows[i].dxgi || (rows[i].dxgi_srgb && UINT(format)==rows[i].dxgi_srgb)))
            return amdgpu_wddm_surface_admit(&rows[i],AMDGPU_WDDM_SURFACE_COMPOSED);
    return nullptr;
}
// The same row, only when the LB7A description carries its D3DDDIFORMAT.
inline const AMDGPU_WDDM_SURFACE_FORMAT *runtime_surface_format(UINT d3dddi,DXGI_FORMAT format) {
    const auto *row=runtime_surface_format(format);
    return row && row->d3dddi==d3dddi ? row : nullptr;
}
// The row a surface may be scanned out as, by the one user-mode rule bc250_scanout_format_admitted:
// a SCANOUT_PRIMARY row of the table, and for a row other than the firmware's own format (RGBA8 and
// RGB10A2 from the kernel driver 0.7.216.20) the trailer's BC250_SCANOUT_CAPS_PLANE_FORMATS as well.
// scanoutCaps is that trailer's flags word; this shell does not read the trailer, so its caller passes
// 0 and only the firmware's BGRA8 row is a candidate here. RGBA16F is COMPOSED only, so an FP16 swap
// chain is never a candidate and keeps the composed-primary path (M14.1) unchanged. An sRGB view shares
// the storage row, as it does for composition.
inline const AMDGPU_WDDM_SURFACE_FORMAT *runtime_scanout_format(DXGI_FORMAT format,unsigned scanoutCaps=0) {
    unsigned count=0;
    const auto *rows=amdgpu_wddm_surface_formats(&count);
    for (unsigned i=0;i<count;++i)
        if (rows[i].dxgi && (UINT(format)==rows[i].dxgi || (rows[i].dxgi_srgb && UINT(format)==rows[i].dxgi_srgb)))
            return bc250_scanout_format_admitted(&rows[i],scanoutCaps)?&rows[i]:nullptr;
    return nullptr;
}
// The pitch of the engine's linear image (RADV on GFX10: a row rounded up to 256 bytes, for
// every pixel size the table has). Width is at most 16384, so the product fits.
inline UINT runtime_surface_pitch(UINT width,UINT bytesPerPixel) { return (width*bytesPerPixel+255)&~255u; }
// A received LB7A v1 description at the row's bytes a pixel, bounded as the kernel driver's type-0
// admission bounds it: whole pixels a row, the row fits the pitch, the size covers every row. A
// producer may pad the pitch and the size. At 4 bytes this is WddmSurfaceGeometry(s,0,4). The rule
// itself lives in driver/contract/bc250_shared_surface.h, so the D3D12 shell applies the same one.
inline bool runtime_surface_geometry(const BC250_WDDM_ALLOCATION_PRIVATE &s,UINT bytesPerPixel) {
    return Bc250SharedSurfaceGeometry(&s,bytesPerPixel)!=0;
}
}
