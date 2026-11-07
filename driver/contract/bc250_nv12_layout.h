/* SPDX-License-Identifier: MIT */
#ifndef BC250_NV12_LAYOUT_H
#define BC250_NV12_LAYOUT_H

/* Explicit linear, progressive NV12 storage. This is allocation metadata, not
 * an overlay capability. Each byte pitch includes padding; UV contains Width/2
 * interleaved two-byte samples. Both components are in the same allocation.
 * No implicit UV offset, modifier, compression or plane allocation is inferred.
 * The bounds are the DCN201 preparation limits in dcn_surface_layout.c. */
typedef struct BC250_NV12_STORAGE {
    unsigned int Width, Height, PitchBytes, ChromaPitchBytes;
    unsigned long long AllocationBytes, SurfaceOffset, ChromaOffset;
} BC250_NV12_STORAGE;

static inline int Bc250Nv12StorageValid(const BC250_NV12_STORAGE* p)
{
    unsigned long long y, uv;
    if (!p || !p->Width || p->Width > 16383u || !p->Height || p->Height > 16383u ||
        (p->Width & 1u) || (p->Height & 1u) || !p->PitchBytes || !p->ChromaPitchBytes ||
        (p->PitchBytes & 255u) || (p->ChromaPitchBytes & 255u) ||
        p->PitchBytes > 16384u || p->ChromaPitchBytes > 32768u ||
        p->PitchBytes < p->Width || p->ChromaPitchBytes < p->Width ||
        (p->SurfaceOffset & 255ull) || (p->ChromaOffset & 255ull)) return 0;
    y = (unsigned long long)p->PitchBytes * p->Height;
    uv = (unsigned long long)p->ChromaPitchBytes * (p->Height / 2u);
    if (p->SurfaceOffset > p->AllocationBytes || y > p->AllocationBytes - p->SurfaceOffset ||
        p->ChromaOffset > p->AllocationBytes || uv > p->AllocationBytes - p->ChromaOffset) return 0;
    return !(p->SurfaceOffset < p->ChromaOffset + uv && p->ChromaOffset < p->SurfaceOffset + y);
}
#endif
