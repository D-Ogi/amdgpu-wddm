// SPDX-License-Identifier: MIT
#ifndef BC250_SURFACE_RESOURCE_PRIVATE_H
#define BC250_SURFACE_RESOURCE_PRIVATE_H
// E26R resource-group contract. Windows ULONG fields, no pointers or compiler
// enums on the wire. v1=12 bytes, v2=16 bytes, v3=64 bytes. Allocation LB7A
// metadata remains separate and unchanged. v3 requires a KMD admitting it.
#define BC250_SURFACE_RESOURCE_MAGIC 0x52363245ul
#define BC250_SURFACE_RESOURCE_TEXTURE_VERSION 3ul
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
    if (words[3]&~3ul) return 0;
    *SharedCpu=(int)words[2];
    *CachedCpu=*SharedCpu && (words[3]&2ul)!=0 && (words[3]&1ul)==0;
    return 1;
}
#endif
