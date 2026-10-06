// SPDX-License-Identifier: MIT
#ifndef BC250_SURFACE_RESOURCE_PRIVATE_H
#define BC250_SURFACE_RESOURCE_PRIVATE_H
// E26R resource-group contract. Windows ULONG fields, no pointers or compiler
// enums on the wire. v1=12 bytes, v2=16 bytes, v3=64 bytes. Allocation LB7A
// metadata remains separate and unchanged. v3 requires a KMD admitting it.
#define BC250_SURFACE_RESOURCE_MAGIC 0x52363245ul
#define BC250_SURFACE_RESOURCE_TEXTURE_VERSION 3ul
// Access intent bits. PRIMARY and CPU_READ are v2's; SCANOUT is M15.14's and means the resource's
// allocations are meant to reach SetVidPnSourceAddress, so they belong in the local segment the
// DirectFlip descriptor names rather than in the shared aperture. It implies PRIMARY and excludes
// CPU_READ: a scanned-out surface has no cached CPU reader.
#define BC250_SURFACE_RESOURCE_PRIMARY 0x1ul
#define BC250_SURFACE_RESOURCE_CPU_READ 0x2ul
#define BC250_SURFACE_RESOURCE_SCANOUT 0x4ul
#define BC250_SURFACE_RESOURCE_ACCESS_MASK 0x7ul
// The only video present source display.c offers and the only one SetVidPnSourceAddress accepts
// (wddm.c). Every flip-eligible primary names it. A mode list is the change that must come first if
// this ever stops being 0, and the DirectFlip rule refuses anything else until then
// (ref/ddi-display/d3d10umddi.md: both allocations of a direct flip share one VidPnSourceId).
#define BC250_SCANOUT_VIDPN_SOURCE 0ul
typedef struct _BC250_SURFACE_RESOURCE_PRIVATE {
    unsigned long Magic, Version, Shared, Access;
    // D3D11_TEXTURE2D_DESC1, serialized explicitly. Format is DXGI_FORMAT.
    unsigned long Width, Height, MipLevels, ArraySize, Format;
    unsigned long SampleCount, SampleQuality, Usage, BindFlags, CpuAccessFlags, MiscFlags, TextureLayout;
} BC250_SURFACE_RESOURCE_PRIVATE;
// KMD consumes policy only; UMD must independently validate the texture fields
// against LB7A geometry and supported API capabilities when opening the resource.
static __inline int Bc250SurfaceResourcePolicy(const void* Data, unsigned int Bytes,
    int* SharedCpu, int* CachedCpu)
{
    const unsigned long* words=(const unsigned long*)Data;
    if (!SharedCpu || !CachedCpu) return 0;
    *SharedCpu=0; *CachedCpu=0;
    if (!Data || Bytes<sizeof(unsigned long) || words[0]!=BC250_SURFACE_RESOURCE_MAGIC) return 1;
    if (Bytes<3*sizeof(unsigned long) || words[2]>1) return 0;
    if (words[1]==1 && Bytes==3*sizeof(unsigned long)) { *SharedCpu=(int)words[2]; return 1; }
    if (!((words[1]==2 && Bytes==4*sizeof(unsigned long)) ||
        (words[1]==BC250_SURFACE_RESOURCE_TEXTURE_VERSION && Bytes==sizeof(BC250_SURFACE_RESOURCE_PRIVATE)))) return 0;
    if (words[3]&~BC250_SURFACE_RESOURCE_ACCESS_MASK) return 0;
    // SCANOUT is a scan-out primary and nothing else: without PRIMARY it is a resource claiming the
    // display pipeline for a surface the runtime never made a primary, and with CPU_READ it is asking
    // for a cached CPU mapping of a plane the display core reads.
    if ((words[3]&BC250_SURFACE_RESOURCE_SCANOUT) &&
        ((words[3]&BC250_SURFACE_RESOURCE_PRIMARY)==0 || (words[3]&BC250_SURFACE_RESOURCE_CPU_READ))) return 0;
    *SharedCpu=(int)words[2];
    *CachedCpu=*SharedCpu && (words[3]&BC250_SURFACE_RESOURCE_CPU_READ)!=0 &&
        (words[3]&BC250_SURFACE_RESOURCE_PRIMARY)==0;
    return 1;
}

// M15.14: whether this resource record asks for scan-out. Shape only, and only for a record this
// parser has already admitted; the caller still decides what placement and what flip that earns.
// A record of no E26R magic, or of v1, has no access word and therefore never asks.
static __inline int Bc250SurfaceResourceScanout(const void* Data, unsigned int Bytes)
{
    const unsigned long* words=(const unsigned long*)Data;
    int shared=0,cached=0;
    if (!Bc250SurfaceResourcePolicy(Data,Bytes,&shared,&cached)) return 0;
    if (!Data || Bytes<4*sizeof(unsigned long) || words[0]!=BC250_SURFACE_RESOURCE_MAGIC) return 0;
    return (words[3]&BC250_SURFACE_RESOURCE_SCANOUT)!=0;
}

// M15.14 increment 2: the record's own version and access words, for a driver that logs what the
// creator asked for and for a reader that must tell a v1 record from a v3 one. Both are 0 for a record
// with no E26R magic and the access word is 0 for v1, which has none. Shape only, and only for a record
// this parser has already admitted: 0 means the record was refused and nothing was written.
static __inline int Bc250SurfaceResourceIntent(const void* Data, unsigned int Bytes,
    unsigned long* Version, unsigned long* Access)
{
    const unsigned long* words=(const unsigned long*)Data;
    int shared=0,cached=0;
    if (!Version || !Access) return 0;
    *Version=0; *Access=0;
    if (!Bc250SurfaceResourcePolicy(Data,Bytes,&shared,&cached)) return 0;
    if (!Data || Bytes<3*sizeof(unsigned long) || words[0]!=BC250_SURFACE_RESOURCE_MAGIC) return 1;
    *Version=words[1];
    if (Bytes>=4*sizeof(unsigned long)) *Access=words[3];
    return 1;
}
#endif
