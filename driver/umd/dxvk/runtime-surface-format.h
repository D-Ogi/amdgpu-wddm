// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../contract/amdgpu_wddm_surface_format.h"
#include "../../kmd/gdi_private.h"
namespace bc250::umd {
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
// The pitch of the engine's linear image (RADV on GFX10: a row rounded up to 256 bytes, for
// every pixel size the table has). Width is at most 16384, so the product fits.
inline UINT runtime_surface_pitch(UINT width,UINT bytesPerPixel) { return (width*bytesPerPixel+255)&~255u; }
// A received LB7A v1 description at the row's bytes a pixel, bounded as the kernel driver's type-0
// admission bounds it: whole pixels a row, the row fits the pitch, the size covers every row. A
// producer may pad the pitch and the size. At 4 bytes this is WddmSurfaceGeometry(s,0,4).
inline bool runtime_surface_geometry(const BC250_WDDM_ALLOCATION_PRIVATE &s,UINT bytesPerPixel) {
    return s.Magic==BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && s.Version==1 && bytesPerPixel &&
        s.Width && s.Height && s.Pitch && !(s.Pitch%bytesPerPixel) &&
        UINT64(s.Width)*bytesPerPixel<=s.Pitch && s.Size<=~0ull-4095ull && s.Size>=UINT64(s.Pitch)*s.Height;
}
}
