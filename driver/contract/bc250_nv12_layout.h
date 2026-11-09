/* SPDX-License-Identifier: MIT */
#ifndef BC250_NV12_LAYOUT_H
#define BC250_NV12_LAYOUT_H
#define BC250_NV12_D3DDDI_FORMAT 0x3231564eu /* D3DDDIFMT_NV12, checked against WDK by consumers */
#define BC250_NV12_DXGI_FORMAT 103u

/* Explicit linear, progressive NV12 storage. This is allocation metadata, not
 * an overlay capability. Each byte pitch includes padding; UV contains Width/2
 * interleaved two-byte samples. Both components are in the same allocation.
 * No implicit UV offset, modifier, compression or plane allocation is inferred. */
typedef struct BC250_NV12_STORAGE {
    unsigned int Width, Height, PitchBytes, ChromaPitchBytes;
    unsigned long long AllocationBytes, SurfaceOffset, ChromaOffset;
} BC250_NV12_STORAGE;

/* The integrity of the record alone, for any GPU: even, non-zero extent, each
 * pitch holds a visible row, the UV pitch and offset keep two-byte samples
 * whole, both components are inside the allocation and do not overlap. No
 * display-engine limit is here: a user-mode driver takes the pitch, offset and
 * alignment that the device's own image layout query gives (another AMD GPU
 * pads rows to another boundary), and only the code that programs a display
 * engine applies that engine's limits (BC-250: Bc250Nv12LayoutPrepare in
 * driver/kmd/dcn_surface_layout.c). */
static inline int Bc250Nv12StorageIntegrity(const BC250_NV12_STORAGE* p)
{
    unsigned long long y, uv;
    if (!p || !p->Width || !p->Height || (p->Width & 1u) || (p->Height & 1u) ||
        !p->PitchBytes || !p->ChromaPitchBytes ||
        p->PitchBytes < p->Width || p->ChromaPitchBytes < p->Width ||
        (p->ChromaPitchBytes & 1u) || (p->ChromaOffset & 1ull)) return 0;
    y = (unsigned long long)p->PitchBytes * p->Height;
    uv = (unsigned long long)p->ChromaPitchBytes * (p->Height / 2u);
    if (p->SurfaceOffset > p->AllocationBytes || y > p->AllocationBytes - p->SurfaceOffset ||
        p->ChromaOffset > p->AllocationBytes || uv > p->AllocationBytes - p->ChromaOffset) return 0;
    return !(p->SurfaceOffset < p->ChromaOffset + uv && p->ChromaOffset < p->SurfaceOffset + y);
}

#endif
