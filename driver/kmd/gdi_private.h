#ifndef BC250_GDI_PRIVATE_H
#define BC250_GDI_PRIVATE_H
#include "dcn_translate.h"

// Windows ABI: 32-bit unsigned long, 64-bit unsigned long long.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"
typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    unsigned long Magic;
    unsigned long Version;
    unsigned long Width;
    unsigned long Height;
    unsigned long Pitch;
    unsigned long Format;                       // D3DDDIFORMAT
    unsigned long long Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

// Optional standard-GDI trailer. Keep the 32-byte LB7A v1 prefix unchanged:
// deployed UMD OpenResource accepts >=32 bytes and consumes only that prefix.
#define BC250_GDI_PRIVATE_MAGIC 0x31494447ul // "GDI1"
typedef struct _BC250_GDI_PRIVATE {
    BC250_WDDM_ALLOCATION_PRIVATE Surface;
    unsigned long Magic, Type, Flags, Reserved;
} BC250_GDI_PRIVATE;

// Allocation admission is narrower than layout arithmetic. Existing-system
// memory and cross-adapter ownership are not implemented; reserved types are
// not admitted merely because their dimensions can be described.
typedef struct _BC250_GDI_ALLOCATION_POLICY {
    int CpuVisible, Aperture, Cached, AccessedPhysically;
} BC250_GDI_ALLOCATION_POLICY;
static __inline int WddmGdiAllocationPolicy(unsigned long Type,int SharedCpu,int CachedCpu,
    BC250_GDI_ALLOCATION_POLICY* Policy)
{
    BC250_GDI_ALLOCATION_POLICY result;
    if (!Policy || Type>4) return 0;
    result.CpuVisible=!Type || Type==2;
    result.Aperture=Type ? Type==2 : SharedCpu!=0;
    result.Cached=Type ? Type==2 : (CachedCpu && result.Aperture);
    result.AccessedPhysically=!Type && !result.Aperture;
    *Policy=result;
    return 1;
}

static __inline int WddmGdiPrivate(const void* Data, unsigned int Bytes, unsigned long* Type)
{
    const BC250_GDI_PRIVATE* gdi=(const BC250_GDI_PRIVATE*)Data;
    if (!Type) return 0;
    *Type=0;
    if (!Data) return 0;
    if (Bytes==sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) return 1;
    if (Bytes!=sizeof(*gdi) || gdi->Magic!=BC250_GDI_PRIVATE_MAGIC ||
        gdi->Surface.Version!=1 || !gdi->Type || gdi->Type>8 ||
        gdi->Flags || gdi->Reserved) return 0;
    *Type=gdi->Type;
    return 1;
}

/* Type values are the WDK GDISURFACETYPE ABI (checked by the kernel caller).
 * A tag describes requested policy, never OS provenance or backing identity. */
static __inline int WddmGdiLayout(unsigned long Width,unsigned long Height,
    unsigned long Type,unsigned long Bpp,unsigned long* Pitch,unsigned long long* Size)
{
    if (Bpp!=1 && Bpp!=4) return 0;
    if (Type==4) return Bpp==1 && DcnStagingLayout(Width,Height,1,Pitch,Size);
    if (Type==2 || Type==3) return DcnStagingLayout(Width,Height,Bpp,Pitch,Size);
    if (Bpp!=4) return 0;
    if (Type==1) return DcnSharedTextureLayout(Width,Height,Pitch,Size);
    return DcnStagingLayout(Width,Height,4,Pitch,Size);
}

static __inline int WddmSurfaceGeometry(const BC250_WDDM_ALLOCATION_PRIVATE* Surface,
    unsigned long Type,unsigned long Bpp)
{
    unsigned long pitch;
    unsigned long long size;
    if (!Surface || Surface->Magic!=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC ||
        Surface->Version!=1 || Type>8 || Surface->Size>~0ull-4095ull) return 0;
    if (!Type) {
        /* Legacy UMD producers may add pitch/row padding. Bound the footprint,
         * but do not require the standard-GDI producer's exact layout. */
        return Bpp==4 && DcnSurfaceBytes(Surface->Width,Surface->Height,Surface->Pitch,&size) &&
            Surface->Size>=size;
    }
    return WddmGdiLayout(Surface->Width,Surface->Height,Type,Bpp,&pitch,&size) &&
        Surface->Pitch==pitch && Surface->Size==size;
}
#endif
