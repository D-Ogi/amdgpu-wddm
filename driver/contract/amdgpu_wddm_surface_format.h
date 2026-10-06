/* SPDX-License-Identifier: MIT */
/*
 * driver/contract/amdgpu_wddm_surface_format.h - the formats a linear surface may have, and what each
 * component may do with a surface of that format.
 *
 * A linear surface is described on the wire by LB7A v1 (driver/kmd/gdi_private.h): width, height,
 * pitch, a D3DDDIFORMAT and a size. The description carries the format already; what used to limit
 * it to 8-bit colour were the readers, each with its own whitelist and its own `width * 4`. Every
 * reader now asks this table instead: the D3D12 shell and engine-ddi when they build the
 * description, the kernel driver when it creates and opens the allocation, the compositor's UMD
 * when it opens and samples the surface.
 *
 * Plain integers only, so C under the miniport's /kernel flags, C++ in the UMDs and C in the Mesa
 * winsys can include it. The values are DXGI_FORMAT and D3DDDIFORMAT numbers; every consumer that
 * has the SDK/WDK headers checks them there with static asserts, none are trusted by eye.
 *
 * Naming trap: DXGI names list channels from the lowest bits, D3DDDIFORMAT names from the highest.
 * DXGI R10G10B10A2_UNORM is D3DDDIFMT_A2B10G10R10 (31), not A2R10G10B10 (35), exactly as DXGI
 * R8G8B8A8_UNORM is D3DDDIFMT_A8B8G8R8 (32).
 *
 * The policy bits are the only place that says a format is enabled for a stage:
 *   COMPOSED         may be a swap-chain buffer or another shared surface the compositor opens (a
 *                    DirectComposition surface or atlas): allocated, opened and sampled by the
 *                    compositor, which converts it to the desktop's format. The monitor's depth does
 *                    not matter.
 *   SCANOUT_PRIMARY  may be the argument of SetVidPnSourceAddress, i.e. read by the display
 *                    pipeline. Today only the formats of the plane the firmware left.
 * A row without a bit is known and refused with a reason, never mistaken for an unknown format.
 *
 * RGBA16F (an FP16 swap chain, scRGB when the application says so) is COMPOSED at 8 bytes a
 * pixel. Every reader takes bytes_per_pixel from the row, never 4: the pitch is at least
 * width * 8. The colour space and HDR metadata are not part of LB7A: DXGI hands them to the
 * compositor, which converts to the desktop's format; no reader here interprets the values.
 * A component built against a copy of this table without the bit still refuses the row (the
 * kernel driver up to 0.7.184.1 and the desktop UMDs 4176D1DF and E6B944CF do), so the bit takes
 * effect on a machine only when every component along the path carries it.
 *
 * A8 (DXGI A8_UNORM, D3DDDIFMT_A8) is COMPOSED at 1 byte a pixel. DirectComposition and XAML keep
 * glyph and mask atlases in it and share them with the compositor: Task Manager asks the D3D11
 * driver for a shared 32x32 A8 render target and recreates its device when that fails (M14.1). It
 * is never a swap-chain format, so the swap-chain paths do not meet it. The rule above holds: the
 * kernel driver up to 0.7.194.1 and the desktop UMDs 18BFC610 and 10D9C983 refuse the row.
 */
#ifndef AMDGPU_WDDM_SURFACE_FORMAT_H
#define AMDGPU_WDDM_SURFACE_FORMAT_H

#if defined(__cplusplus)
extern "C" {
#endif

#define AMDGPU_WDDM_SURFACE_COMPOSED        0x1u
#define AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY 0x2u

/* DXGI_FORMAT values (dxgiformat.h). */
#define AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT  10u
#define AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM   24u
#define AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM      28u
#define AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB 29u
#define AMDGPU_WDDM_DXGI_A8_UNORM            65u
#define AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM      87u
#define AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB 91u

/* D3DDDIFORMAT values (d3dukmdt.h). */
#define AMDGPU_WDDM_D3DDDI_A8R8G8B8      21u
#define AMDGPU_WDDM_D3DDDI_X8R8G8B8      22u
#define AMDGPU_WDDM_D3DDDI_A8            28u
#define AMDGPU_WDDM_D3DDDI_A2B10G10R10   31u
#define AMDGPU_WDDM_D3DDDI_A8B8G8R8      32u
#define AMDGPU_WDDM_D3DDDI_A16B16G16R16F 113u

typedef struct AMDGPU_WDDM_SURFACE_FORMAT {
    unsigned int dxgi;            /* storage format; 0 for a row the kernel/GDI side only uses */
    unsigned int dxgi_srgb;       /* the sRGB view the storage may also be viewed as; 0 if none */
    unsigned int d3dddi;          /* LB7A.Format */
    unsigned int bytes_per_pixel;
    unsigned int policy;          /* AMDGPU_WDDM_SURFACE_* */
    const char *name;             /* for refusal logs */
} AMDGPU_WDDM_SURFACE_FORMAT;

static inline const AMDGPU_WDDM_SURFACE_FORMAT *amdgpu_wddm_surface_formats(unsigned int *count)
{
    static const AMDGPU_WDDM_SURFACE_FORMAT rows[] = {
        {AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM, AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB, AMDGPU_WDDM_D3DDDI_A8R8G8B8, 4,
         AMDGPU_WDDM_SURFACE_COMPOSED | AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY, "BGRA8"},
        {AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM, AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB, AMDGPU_WDDM_D3DDDI_A8B8G8R8, 4,
         AMDGPU_WDDM_SURFACE_COMPOSED, "RGBA8"},
        {AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM, 0, AMDGPU_WDDM_D3DDDI_A2B10G10R10, 4,
         AMDGPU_WDDM_SURFACE_COMPOSED, "RGB10A2"},
        {AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT, 0, AMDGPU_WDDM_D3DDDI_A16B16G16R16F, 8,
         AMDGPU_WDDM_SURFACE_COMPOSED, "RGBA16F"},
        {AMDGPU_WDDM_DXGI_A8_UNORM, 0, AMDGPU_WDDM_D3DDDI_A8, 1, AMDGPU_WDDM_SURFACE_COMPOSED, "A8"},
        {0, 0, AMDGPU_WDDM_D3DDDI_X8R8G8B8, 4, AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY, "X8"},
    };
    *count = (unsigned int)(sizeof(rows) / sizeof(rows[0]));
    return rows;
}

/* The row for a storage format, or 0. 0 is DXGI_FORMAT_UNKNOWN and matches nothing. */
static inline const AMDGPU_WDDM_SURFACE_FORMAT *amdgpu_wddm_surface_format_by_dxgi(unsigned int dxgi)
{
    unsigned int count, i;
    const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
    if (!dxgi)
        return 0;
    for (i = 0; i < count; ++i)
        if (rows[i].dxgi == dxgi)
            return &rows[i];
    return 0;
}

/* The row for an LB7A.Format, or 0. */
static inline const AMDGPU_WDDM_SURFACE_FORMAT *amdgpu_wddm_surface_format_by_d3dddi(unsigned int d3dddi)
{
    unsigned int count, i;
    const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
    for (i = 0; i < count; ++i)
        if (rows[i].d3dddi == d3dddi)
            return &rows[i];
    return 0;
}

/* The row if it is enabled for every bit of stage, otherwise 0. */
static inline const AMDGPU_WDDM_SURFACE_FORMAT *amdgpu_wddm_surface_admit(const AMDGPU_WDDM_SURFACE_FORMAT *row,
                                                                           unsigned int stage)
{
    return row && stage && (row->policy & stage) == stage ? row : 0;
}

#if defined(__cplusplus)
}
#endif

#endif /* AMDGPU_WDDM_SURFACE_FORMAT_H */
