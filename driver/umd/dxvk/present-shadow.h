// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-surface.h"
#include "runtime-surface-format.h"
namespace bc250::umd {
// BD-065: the windowed blt model (DXGI_SWAP_EFFECT_DISCARD or SEQUENTIAL). DXGI presents a buffer of such a swap
// chain with Flags.Blt and no destination; dxgkrnl then names the window's redirection surface, which the
// compositor keeps in B8G8R8A8, and the kernel driver copies with CP DMA. CP DMA moves bytes: it cannot swap red
// and blue, so the kernel driver refuses an R8G8B8A8 source and the window stays empty (session 353, Stygian, a
// Unity 2017 game). The shell therefore converts such a buffer on the GPU into a B8G8R8A8 shadow it owns, and
// presents the shadow. Flip-model buffers reach the compositor without a kernel copy and are left alone.

// BD-253: DXGI can also name the destination: the window's redirection surface, which the runtime opened as a GDI
// texture without a resource record (GOG Galaxy, Qt 6). The kernel driver compares the D3DDDI formats of the two
// allocations and refuses a difference in the same way, so such a present goes through the shadow when the shadow
// has the destination's D3DDDI format.

// The format a windowed Blt present of a buffer in this format is converted to, or DXGI_FORMAT_UNKNOWN when the
// buffer is presented as it is: a flip, a flip-model buffer (shared, primary or displayable: bltModelBuffer false),
// a buffer already in B8G8R8A8, or a named destination of another D3DDDI format than the shadow (destination is the
// DXGI format of the named destination, DXGI_FORMAT_UNKNOWN when there is none or its format is not known). Only
// the 8-bit red-first rows are converted, each to the B8G8R8A8 row of the same encoding.
inline DXGI_FORMAT present_shadow_format(DXGI_DDI_PRESENT_FLAGS flags,bool named,DXGI_FORMAT destination,
    bool bltModelBuffer,DXGI_FORMAT buffer) {
    if (!flags.Blt || flags.Flip || !bltModelBuffer) return DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT shadow=DXGI_FORMAT_UNKNOWN;
    switch (buffer) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: shadow=DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: shadow=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    default: return DXGI_FORMAT_UNKNOWN;
    }
    if (!named) return shadow;
    const auto *target=runtime_surface_format(destination),*row=runtime_surface_format(shadow);
    return target && row && target->d3dddi==row->d3dddi ? shadow : DXGI_FORMAT_UNKNOWN;
}

struct PresentShadowSlot {
    RuntimeSurface *surface=nullptr; // owned by DeviceOwner's surface list, closed with the device
    UINT64 retire=0;                 // the Present fence value of the last Present that read it
};
// Two slots, used in turn: the engine writes one while the kernel driver may still read the other.
struct PresentShadows { PresentShadowSlot slots[2]{}; UINT next=0; };

// The LB7A request and texture of a shadow: a device allocation the shell owns (no runtime resource), not shared,
// not a primary, a render target and shader resource so that the engine's Blt can blit or draw into it. The
// geometry is that of convert_runtime_resource: the engine's linear pitch, rows rounded up to 4, whole pages.
inline HRESULT present_shadow_request(UINT width,UINT height,DXGI_FORMAT format,
    RuntimeSurfaceRequest &request,D3D11_TEXTURE2D_DESC1 &desc) {
    const auto *row=runtime_surface_format(format);
    if (!row || row->bytes_per_pixel!=4 || !width || !height || width>16384 || height>16384) return E_INVALIDARG;
    const UINT pitch=runtime_surface_pitch(width,row->bytes_per_pixel);
    const UINT64 bytes=(UINT64(pitch)*((height+3)&~3u)+4095)&~UINT64(4095);
    D3D11_TEXTURE2D_DESC1 d{width,height,1,1,format,{1,0},D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    RuntimeSurfaceRequest r{};
    r.no_cpu_access=true; // Private GPU Blt destination and kernel Present source, no CPU or opener.
    r.surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,width,height,pitch,row->d3dddi,bytes};
    r.texture={BC250_SURFACE_RESOURCE_MAGIC,BC250_SURFACE_RESOURCE_TEXTURE_VERSION,0u,0u,
        d.Width,d.Height,d.MipLevels,d.ArraySize,UINT(d.Format),d.SampleDesc.Count,d.SampleDesc.Quality,
        UINT(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags,UINT(d.TextureLayout)};
    request=r; desc=d; return S_OK;
}
}
