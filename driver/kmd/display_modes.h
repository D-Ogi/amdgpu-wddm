#ifndef BC250_DISPLAY_MODES_H
#define BC250_DISPLAY_MODES_H
#include "surface_format.h"

// The pixel formats of the VidPN source modes that display.c offers for the one inherited geometry. Plain
// integers, so driver\kmd\test\display_modes_test.c compiles this without a WDK header.
//
// dxgkrnl builds D3DKMTGetDisplayModeList from the source mode sets, one entry per source mode and target mode,
// each with the source mode's D3DDDIFORMAT (D3DKMT_DISPLAYMODE.Format, d3dkmthk.h). We expect DXGI's
// GetDisplayModeList(Format) to return the entries with the D3DDDIFORMAT of Format (R8G8B8A8_UNORM is A8B8G8R8,
// R10G10B10A2_UNORM is A2B10G10R10, R16G16B16A16_FLOAT is A16B16G16R16F; amdgpu_wddm_surface_format.h). This
// is not measured yet: tools\win\dxgimodes prints both lists side by side. Up to 0.7.200.1 the set held
// A8R8G8B8 only, and 3DMark Steel Nomad stopped with "Display mode list not found for given format" (lab
// session native-caps349).
//
// The list comes from the shared table, never from a second whitelist:
//   1. the SCANOUT_PRIMARY rows that DXGI can name (dxgi != 0): the plane the firmware left, BGRA8;
//   2. with Composed, the COMPOSED rows that DXGI can name of 4 bytes a pixel or more: RGBA8, RGB10A2, RGBA16F.
// The size bound keeps the atlas rows out of the display mode list. A source mode's Stride scales the
// firmware's 4-byte pitch (Bc250SourceModeStride below), and A8, the 1-byte DirectComposition and XAML
// glyph and mask format, is never a swap-chain format (driver\contract\amdgpu_wddm_surface_format.h), so a
// display mode of it would name geometry no swap chain can ask for.
// A mode of step 2 does not change the scan-out. The display keeps the 8-bit plane, SetVidPnSourceAddress keeps
// refusing allocations without SCANOUT_PRIMARY (wddm.c), and the UMDs present such buffers through composition.
// The first entry is the GDI format, so a consumer that takes the first mode gets the desktop's format.
// Composed is FALSE for the display-only table, which copies 4-byte pixels by CPU (display.c CopyRect).
#define BC250_SOURCE_MODE_MAX 8ul

static __inline unsigned long Bc250SourceModeFormats(int Composed, unsigned long* Formats, unsigned long Max)
{
    unsigned int count, i, pass;
    unsigned long n = 0;
    const AMDGPU_WDDM_SURFACE_FORMAT* rows = amdgpu_wddm_surface_formats(&count);
    for (pass = 0; pass < 2; ++pass) {
        if (pass == 1 && !Composed) break;
        for (i = 0; i < count; ++i) {
            const int scanout = amdgpu_wddm_surface_admit(&rows[i], AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY) != 0;
            const int composed = amdgpu_wddm_surface_admit(&rows[i], AMDGPU_WDDM_SURFACE_COMPOSED) != 0;
            if (!rows[i].dxgi || rows[i].bytes_per_pixel < 4) continue;
            if (pass == 0 ? !scanout : (scanout || !composed)) continue;
            if (n < Max) Formats[n] = rows[i].d3dddi;
            ++n;
        }
    }
    return n;
}

// Nonzero when a source mode of Format is one that Bc250SourceModeFormats offers.
static __inline int Bc250SourceModeAdmitted(int Composed, unsigned long Format)
{
    unsigned long formats[BC250_SOURCE_MODE_MAX], n, i;
    n = Bc250SourceModeFormats(Composed, formats, BC250_SOURCE_MODE_MAX);
    for (i = 0; i < n && i < BC250_SOURCE_MODE_MAX; ++i)
        if (formats[i] == Format) return 1;
    return 0;
}

// The Stride of a source mode of Format: the firmware's pitch scaled from 4 bytes a pixel to the row's size, so
// the padding of the inherited plane is kept. 0 when the format is unknown or the pitch does not divide or fit.
static __inline unsigned long Bc250SourceModeStride(unsigned long Format, unsigned long PostPitch)
{
    const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_format_by_d3dddi((unsigned int)Format);
    unsigned long pixels;
    if (!row || !row->bytes_per_pixel || PostPitch == 0 || (PostPitch & 3ul)) return 0;
    pixels = PostPitch / 4ul;
    if (pixels > 0xfffffffful / row->bytes_per_pixel) return 0;
    return pixels * row->bytes_per_pixel;
}

#endif
